/* A DNS stub resolver (RFC 1035): A records over UDP, asked of one
 * recursive server -- dns= on the command line, the one DHCP offered,
 * or else the gateway. A small cache keeps answers for their TTL (at
 * most an hour). CNAME chains are followed within one answer, which is
 * how recursive servers return them. */
#include "dns.h"
#include <stdbool.h>
#include "cpu.h"
#include "kerrno.h"
#include "kstring.h"
#include "mutex.h"
#include "net.h"
#include "timer.h"
#include "udp.h"

#define DNS_PORT 53
#define TRIES 3
#define WAIT_MS 2000
#define CACHE 8
#define TTL_MAX 3600

uint32_t net_dns;
bool net_dns_from_cmdline;

struct cached {
	char name[DNS_NAME_MAX + 1];
	uint32_t ip;
	uint64_t until; /* uptime ms */
};
static struct cached cache[CACHE];
static unsigned cache_next;
static struct mutex lock = MUTEX_INIT;
static uint16_t query_id;

/* The question's name in DNS form (length-prefixed labels): its length,
 * or 0 if the name is malformed. */
static size_t encode_name(const char *name, uint8_t *out, size_t max)
{
	size_t o = 0;
	const char *s = name;
	while (*s) {
		const char *e = s;
		while (*e && *e != '.') {
			e++;
		}
		size_t n = (size_t)(e - s);
		if (n == 0 || n > 63 || o + 1 + n + 1 > max) {
			return 0;
		}
		out[o++] = (uint8_t)n;
		memcpy(out + o, s, n);
		o += n;
		s = *e ? e + 1 : e; /* a trailing dot ends the name too */
	}
	if (!o) {
		return 0;
	}
	out[o++] = 0;
	return o;
}

/* Skips a (possibly compressed) name at p; the offset after it, or 0. */
static size_t skip_name(const uint8_t *msg, size_t len, size_t p)
{
	for (int labels = 0; p < len && labels < 128; labels++) {
		uint8_t n = msg[p];
		if (n == 0) {
			return p + 1;
		}
		if ((n & 0xC0) == 0xC0) {
			return p + 2 <= len ? p + 2 : 0;
		}
		if (n & 0xC0) {
			return 0;
		}
		p += 1 + (size_t)n;
	}
	return 0;
}

/* The first A record in an answer to our query: true and *ip, *ttl. */
static bool parse_answer(const uint8_t *msg, size_t len, uint16_t id, uint32_t *ip, uint32_t *ttl,
                         int *rcode)
{
	if (len < 12 || get_be16(msg) != id || !(msg[2] & 0x80)) {
		return false;
	}
	*rcode = msg[3] & 0x0F;
	uint16_t qd = get_be16(msg + 4), an = get_be16(msg + 6);
	size_t p = 12;
	for (uint16_t i = 0; i < qd; i++) {
		p = skip_name(msg, len, p);
		if (!p || p + 4 > len) {
			return false;
		}
		p += 4;
	}
	for (uint16_t i = 0; i < an; i++) {
		p = skip_name(msg, len, p);
		if (!p || p + 10 > len) {
			return false;
		}
		uint16_t type = get_be16(msg + p), class = get_be16(msg + p + 2);
		uint32_t t = get_be32(msg + p + 4);
		uint16_t rdlen = get_be16(msg + p + 8);
		p += 10;
		if (p + rdlen > len) {
			return false;
		}
		if (type == 1 && class == 1 && rdlen == 4) {
			*ip = get_be32(msg + p);
			*ttl = t;
			return true;
		}
		p += rdlen; /* a CNAME or other record: its A follows */
	}
	return true; /* an answer, but no address in it */
}

static bool cached(const char *name, uint32_t *ip)
{
	uint64_t now = timer_uptime_ms();
	for (unsigned i = 0; i < CACHE; i++) {
		if (cache[i].name[0] && now < cache[i].until && !strcmp(cache[i].name, name)) {
			*ip = cache[i].ip;
			return true;
		}
	}
	return false;
}

int dns_resolve(const char *name, uint32_t *ip)
{
	if (ip_parse(name, ip, 0)) {
		return 0;
	}
	size_t nlen = strlen(name);
	if (!nlen || nlen > DNS_NAME_MAX) {
		return -EINVAL;
	}
	mutex_lock(&lock);
	if (cached(name, ip)) {
		mutex_unlock(&lock);
		return 0;
	}
	uint32_t server = net_dns;
	if (!server) {
		struct netif *ifc = netif_first();
		for (; ifc && !server; ifc = ifc->next) {
			server = ifc->loopback ? 0 : ifc->gateway;
		}
	}
	if (!server) {
		mutex_unlock(&lock);
		return -ENETUNREACH;
	}
	static uint8_t q[12 + DNS_NAME_MAX + 2 + 4], r[512];
	uint16_t id = (uint16_t)(timer_uptime_ms() * 31u + ++query_id);
	memset(q, 0, 12);
	put_be16(q, id);
	q[2] = 0x01; /* recursion desired */
	put_be16(q + 4, 1);
	size_t qn = encode_name(name, q + 12, sizeof(q) - 12 - 4);
	if (!qn) {
		mutex_unlock(&lock);
		return -EINVAL;
	}
	put_be16(q + 12 + qn, 1); /* A */
	put_be16(q + 14 + qn, 1); /* IN */
	int err;
	struct udp_endpoint *ep = udp_open(0, &err);
	if (!ep) {
		mutex_unlock(&lock);
		return err;
	}
	int rc = -ETIMEDOUT;
	for (int t = 0; t < TRIES && rc == -ETIMEDOUT; t++) {
		if (udp_send(ep, server, DNS_PORT, q, 16 + qn) < 0) {
			continue;
		}
		uint64_t until = timer_uptime_ms() + WAIT_MS;
		for (;;) {
			uint64_t now = timer_uptime_ms();
			if (now >= until) {
				break;
			}
			uint32_t src;
			uint16_t sport;
			long n = udp_recv(ep, r, sizeof(r), &src, &sport, (uint32_t)(until - now));
			if (n < 0 || src != server || sport != DNS_PORT) {
				continue;
			}
			uint32_t addr = 0, ttl = 0;
			int rcode = 0;
			if (!parse_answer(r, (size_t)n, id, &addr, &ttl, &rcode)) {
				continue;
			}
			if (rcode == 3 || (!rcode && !addr)) {
				rc = -ENOENT; /* no such name, or no address for it */
			} else if (rcode) {
				rc = -EIO;
			} else {
				*ip = addr;
				struct cached *c = &cache[cache_next++ % CACHE];
				memcpy(c->name, name, nlen + 1);
				c->ip = addr;
				c->until = timer_uptime_ms() + (uint64_t)(ttl > TTL_MAX ? TTL_MAX : ttl) * 1000;
				rc = 0;
			}
			break;
		}
	}
	udp_close(ep);
	mutex_unlock(&lock);
	return rc;
}
