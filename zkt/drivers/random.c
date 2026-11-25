/* Device "random" (random.h).
 *
 * Every interrupt folds a sample -- the PIT's count within the tick, the
 * TSC's low half, the line, a running count -- into a ring of 64 words
 * (cheap enough for the interrupt path). A read hashes the pool, the
 * ring and a counter into a key, takes its output from SHA-256 of that
 * key and a block number, then replaces the pool with a hash of the key
 * that is never given out: what was read can't be turned back into what
 * comes next. Reads never block; by the time anything runs that wants
 * keys, the timer, disks and keyboard have stirred it many times. */
#include "random.h"
#include "clock.h"
#include "cpuinfo.h"
#include "cpu.h"
#include "device.h"
#include "kstring.h"
#include "rtc.h"
#include "sha256.h"
#include "timer.h"

#define RING 64

static uint32_t ring[RING];
static uint32_t samples;
static uint8_t pool[SHA256_BYTES];
static uint32_t reads;

static inline uint32_t rotl(uint32_t x, unsigned n)
{
	return (x << (n & 31)) | (x >> ((32 - n) & 31));
}

void random_irq(unsigned irq)
{
	uint32_t s = clock_fine() ^ (cpu_cycles_low() * 2654435761u) ^ (irq << 24) ^ samples;
	uint32_t i = samples++ % RING;
	ring[i] = rotl(ring[i], 7) ^ s;
	ring[(i + 13) % RING] += rotl(s, i);
}

void random_add(const void *data, size_t len)
{
	uint32_t flags = cpu_irq_save();
	struct sha256 c;
	sha256_init(&c);
	sha256_update(&c, pool, sizeof(pool));
	sha256_update(&c, data, len);
	sha256_final(&c, pool);
	cpu_irq_restore(flags);
}

void random_bytes(void *buf, size_t len)
{
	uint8_t key[SHA256_BYTES];
	uint32_t flags = cpu_irq_save();
	uint32_t extra[4] = { clock_fine(), cpu_cycles_low(), (uint32_t)timer_uptime_ms(), ++reads };
	struct sha256 c;
	sha256_init(&c);
	sha256_update(&c, pool, sizeof(pool));
	sha256_update(&c, ring, sizeof(ring));
	sha256_update(&c, &samples, sizeof(samples));
	sha256_update(&c, extra, sizeof(extra));
	sha256_final(&c, key);
	/* The next pool: from the key, by a path no output takes. */
	sha256_init(&c);
	sha256_update(&c, "pool", 4);
	sha256_update(&c, key, sizeof(key));
	sha256_final(&c, pool);
	cpu_irq_restore(flags);

	uint8_t *out = buf, block[SHA256_BYTES];
	for (uint32_t n = 0; len; n++) {
		sha256_init(&c);
		sha256_update(&c, "out", 3);
		sha256_update(&c, key, sizeof(key));
		sha256_update(&c, &n, sizeof(n));
		sha256_final(&c, block);
		size_t take = len < sizeof(block) ? len : sizeof(block);
		memcpy(out, block, take);
		out += take;
		len -= take;
	}
	memset(key, 0, sizeof(key));
	memset(block, 0, sizeof(block));
}

static long random_read(struct device *dev, void *buf, size_t len)
{
	(void)dev;
	if (len > 4096) {
		len = 4096;
	}
	random_bytes(buf, len);
	return (long)len;
}

/* Writing mixes what is written into the pool (it can't be read back). */
static long random_write(struct device *dev, const void *buf, size_t len)
{
	(void)dev;
	random_add(buf, len);
	return (long)len;
}

static const struct char_device_ops random_ops = { .read = random_read, .write = random_write };
static struct device random_device = { .name = "random", .class = DEVICE_CHAR, .char_ops = &random_ops };

void random_register(void)
{
	uint32_t seed[4] = { rtc_time(), clock_fine(), cpu_cycles_low(), (uint32_t)(uintptr_t)&seed };
	random_add(seed, sizeof(seed));
	device_register(&random_device);
}
