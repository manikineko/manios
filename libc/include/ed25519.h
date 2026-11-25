/* Ed25519 signature verification (RFC 8032) and SHA-512 (FIPS 180-4),
 * for checking signed package indexes (pkg). Verification only: ManiOS
 * holds no signing keys. */
#ifndef MANIOS_ED25519_H
#define MANIOS_ED25519_H

#include <stddef.h>
#include <stdint.h>

#define ED25519_PUBLIC_KEY 32
#define ED25519_SIGNATURE 64
#define SHA512_BYTES 64

struct sha512 {
	uint64_t h[8];
	uint8_t block[128];
	size_t used;
	uint64_t bytes;
};

void sha512_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha512_final(struct sha512 *c, uint8_t out[SHA512_BYTES]);

/* 0 if sig is a valid signature of msg under pk, else -1. */
/* 0.31: the public key of a 32-byte secret seed, and a signature with
 * it (RFC 8032: the seed is the private key). */
void ed25519_public_key(const uint8_t seed[32], uint8_t pk[ED25519_PUBLIC_KEY]);
void ed25519_sign(uint8_t sig[ED25519_SIGNATURE], const void *msg, size_t len, const uint8_t seed[32],
                  const uint8_t pk[ED25519_PUBLIC_KEY]);
/* X25519 (RFC 7748): scalar times point; _base: times the base point 9. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

int ed25519_verify(const uint8_t sig[ED25519_SIGNATURE], const void *msg, size_t len,
                   const uint8_t pk[ED25519_PUBLIC_KEY]);

#endif
