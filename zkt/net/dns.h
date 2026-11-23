/* The DNS stub resolver (dns.c). */
#ifndef ZKT_NET_DNS_H
#define ZKT_NET_DNS_H

#include <stdint.h>

#define DNS_NAME_MAX 253

/* A host name, or a dotted quad, to an IPv4 address: 0, or -ENOENT (no
 * such name or no address), -ETIMEDOUT, -ENETUNREACH (no server known),
 * -EINVAL (malformed), -EIO (the server failed). May wait seconds. */
int dns_resolve(const char *name, uint32_t *ip);

#endif
