/* A minimal HTTP/1.1 client (RFC 9110/9112): one GET per connection,
 * "Connection: close", bodies delimited by Content-Length, chunked
 * transfer coding, or the end of the connection. */
#include <errno.h>
#include <http.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_MAX 4096
#define BUF 4096

struct conn {
	int fd;
	unsigned char buf[BUF];
	size_t pos, len;
	int eof;
};

static int fill(struct conn *c)
{
	if (c->pos < c->len) {
		return 1;
	}
	if (c->eof) {
		return 0;
	}
	long n = read(c->fd, c->buf, sizeof(c->buf));
	if (n <= 0) {
		c->eof = 1;
		return n < 0 ? -1 : 0;
	}
	c->pos = 0;
	c->len = (size_t)n;
	return 1;
}

/* A line of at most max-1 characters, without its CRLF: its length, or
 * -1 at end of stream or if too long. */
static long getline_crlf(struct conn *c, char *line, size_t max)
{
	size_t n = 0;
	for (;;) {
		int r = fill(c);
		if (r <= 0) {
			return -1;
		}
		char ch = (char)c->buf[c->pos++];
		if (ch == '\n') {
			if (n && line[n - 1] == '\r') {
				n--;
			}
			line[n] = 0;
			return (long)n;
		}
		if (n + 1 >= max) {
			return -1;
		}
		line[n++] = ch;
	}
}

static int write_all(int fd, const char *s, size_t len)
{
	while (len) {
		long n = write(fd, s, len);
		if (n <= 0) {
			return -1;
		}
		s += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Hands the sink up to `want` bytes (want < 0: until the end). */
static int body(struct conn *c, long want, http_sink sink, void *ctx, struct http_result *res)
{
	while (want != 0) {
		int r = fill(c);
		if (r < 0) {
			snprintf(res->error, sizeof(res->error), "connection failed while reading");
			return -1;
		}
		if (r == 0) {
			if (want > 0) {
				snprintf(res->error, sizeof(res->error), "connection closed early");
				return -1;
			}
			return 0;
		}
		size_t n = c->len - c->pos;
		if (want > 0 && (long)n > want) {
			n = (size_t)want;
		}
		if (sink && sink(ctx, c->buf + c->pos, n)) {
			snprintf(res->error, sizeof(res->error), "stopped");
			return -1;
		}
		c->pos += n;
		if (want > 0) {
			want -= (long)n;
		}
	}
	return 0;
}

static int ieq_prefix(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++) {
		char a = *s, b = *prefix;
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) {
			return 0;
		}
	}
	return 1;
}

static int once(const char *url, http_sink sink, void *ctx, struct http_result *res)
{
	char host[256], path[512], dialstr[300];
	unsigned port = 80;
	if (strncmp(url, "http://", 7) != 0) {
		snprintf(res->error, sizeof(res->error), "only http:// URLs are supported");
		return -1;
	}
	const char *h = url + 7, *slash = strchr(h, '/');
	size_t hl = slash ? (size_t)(slash - h) : strlen(h);
	if (!hl || hl >= sizeof(host)) {
		snprintf(res->error, sizeof(res->error), "bad host in URL");
		return -1;
	}
	memcpy(host, h, hl);
	host[hl] = 0;
	char *colon = strchr(host, ':');
	if (colon) {
		*colon = 0;
		port = (unsigned)atoi(colon + 1);
		if (!port || port > 65535) {
			snprintf(res->error, sizeof(res->error), "bad port in URL");
			return -1;
		}
	}
	snprintf(path, sizeof(path), "%s", slash ? slash : "/");
	snprintf(dialstr, sizeof(dialstr), "tcp!%s!%u", host, port);

	struct conn *c = calloc(1, sizeof(*c));
	if (!c) {
		snprintf(res->error, sizeof(res->error), "out of memory");
		return -1;
	}
	c->fd = dial(dialstr);
	if (c->fd < 0) {
		snprintf(res->error, sizeof(res->error), "cannot connect to %s:%u: %s", host, port,
		         strerror(errno));
		free(c);
		return -1;
	}
	char req[1024];
	int n = snprintf(req, sizeof(req),
	                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ManiOS-pkg/1\r\n"
	                 "Accept: */*\r\nConnection: close\r\n\r\n",
	                 path, host);
	int rc = -1;
	char line[HEAD_MAX];
	if (n <= 0 || (size_t)n >= sizeof(req) || write_all(c->fd, req, (size_t)n)) {
		snprintf(res->error, sizeof(res->error), "cannot send the request");
		goto out;
	}
	if (getline_crlf(c, line, sizeof(line)) < 0 || strncmp(line, "HTTP/1.", 7) != 0
	    || strlen(line) < 12) {
		snprintf(res->error, sizeof(res->error), "not an HTTP response");
		goto out;
	}
	res->status = atoi(line + 9);
	res->length = -1;
	res->location[0] = 0;
	int chunked = 0;
	for (;;) {
		long ln = getline_crlf(c, line, sizeof(line));
		if (ln < 0) {
			snprintf(res->error, sizeof(res->error), "bad response headers");
			goto out;
		}
		if (ln == 0) {
			break;
		}
		if (ieq_prefix(line, "content-length:")) {
			res->length = atol(line + 15);
		} else if (ieq_prefix(line, "transfer-encoding:") && strstr(line, "chunked")) {
			chunked = 1;
		} else if (ieq_prefix(line, "location:")) {
			const char *v = line + 9;
			while (*v == ' ') v++;
			snprintf(res->location, sizeof(res->location), "%s", v);
		}
	}
	if (res->status != 200) {
		rc = 0; /* the caller decides; no body delivered */
		goto out;
	}
	if (chunked) {
		for (;;) {
			if (getline_crlf(c, line, sizeof(line)) < 0) {
				snprintf(res->error, sizeof(res->error), "bad chunk");
				goto out;
			}
			long size = strtol(line, 0, 16);
			if (size < 0) {
				snprintf(res->error, sizeof(res->error), "bad chunk size");
				goto out;
			}
			if (size == 0) {
				break;
			}
			if (body(c, size, sink, ctx, res) || getline_crlf(c, line, sizeof(line)) != 0) {
				if (!res->error[0]) snprintf(res->error, sizeof(res->error), "bad chunk end");
				goto out;
			}
		}
		rc = 0;
	} else {
		rc = body(c, res->length, sink, ctx, res);
	}
out:
	close(c->fd);
	free(c);
	return rc;
}

int http_get(const char *url, http_sink sink, void *ctx, struct http_result *res)
{
	char next[256];
	snprintf(next, sizeof(next), "%s", url);
	for (int hop = 0; hop < 4; hop++) {
		memset(res, 0, sizeof(*res));
		if (once(next, sink, ctx, res)) {
			return -1;
		}
		int s = res->status;
		if ((s == 301 || s == 302 || s == 303 || s == 307 || s == 308) && res->location[0]) {
			if (strncmp(res->location, "http://", 7) != 0) {
				snprintf(res->error, sizeof(res->error), "redirected to %.60s (not http://)",
				         res->location);
				return -1;
			}
			snprintf(next, sizeof(next), "%s", res->location);
			continue;
		}
		return 0;
	}
	snprintf(res->error, sizeof(res->error), "too many redirects");
	return -1;
}
