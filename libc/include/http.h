/* A minimal HTTP/1.1 client over dial() (http.c): GET of an http:// URL,
 * with the body handed to a callback as it arrives. Plain HTTP only --
 * ManiOS has no TLS -- so whatever is fetched must be checked by other
 * means (pkg verifies signatures and SHA-256 sums). */
#ifndef MANIOS_HTTP_H
#define MANIOS_HTTP_H

#include <stddef.h>

/* Called for each piece of the body; returns 0 to go on, nonzero to
 * stop (http_get then returns -1 with errno EINTR... set by the caller). */
typedef int (*http_sink)(void *ctx, const void *data, size_t len);

struct http_result {
	int status;            /* 200, 404, ... */
	long length;           /* the body's length, or -1 if not announced */
	char location[256];    /* a redirect's Location, if any */
	char error[96];        /* why it failed, for messages */
};

/* Fetches url ("http://HOST[:PORT]/PATH"), following up to 3 redirects
 * to other http:// URLs. Returns 0 when a response was received (look at
 * res->status: the body was only delivered for 200), or -1 with
 * res->error set. */
int http_get(const char *url, http_sink sink, void *ctx, struct http_result *res);

#endif
