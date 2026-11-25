/* /dev/random (0.31): bytes for keys and nonces, from a pool that every
 * interrupt stirs with when it came (the PIT's count, the TSC where
 * there is one), hashed with SHA-256 on the way out. */
#ifndef ZKT_DRIVERS_RANDOM_H
#define ZKT_DRIVERS_RANDOM_H

#include <stddef.h>

/* From irq_dispatch: cheap, interrupts off. */
void random_irq(unsigned irq);
/* Mixes bytes in (boot time, addresses, anything that differs). */
void random_add(const void *data, size_t len);
/* Fills buf; never blocks. */
void random_bytes(void *buf, size_t len);
void random_register(void);

#endif
