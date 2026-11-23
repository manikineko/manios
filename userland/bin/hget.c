/* hget URL [FILE] -- fetch an http:// URL (HTTP/1.1 over TCP), writing
 * the body to FILE, or to standard output. Plain HTTP only: ManiOS has
 * no TLS. Exit status 0 for 200 OK, 1 otherwise. */
#include <errno.h>
#include <http.h>
#include <manios.h>
#include <stdio.h>
#include <string.h>

static int out_fd = 1;
static unsigned long got;

static int sink(void *ctx, const void *data, size_t len)
{
	(void)ctx;
	const char *p = data;
	while (len) {
		long n = write(out_fd, p, len);
		if (n <= 0) {
			return 1;
		}
		p += n;
		len -= (size_t)n;
		got += (unsigned long)n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: hget URL [FILE]\n");
		return 2;
	}
	if (argc == 3) {
		out_fd = open(argv[2], OWRITE | O_CREAT | O_TRUNC);
		if (out_fd < 0) {
			fprintf(stderr, "hget: %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
	}
	struct http_result res;
	int rc = http_get(argv[1], sink, 0, &res);
	if (argc == 3) {
		close(out_fd);
	}
	if (rc) {
		fprintf(stderr, "hget: %s: %s\n", argv[1], res.error);
		return 1;
	}
	if (res.status != 200) {
		fprintf(stderr, "hget: %s: HTTP %d\n", argv[1], res.status);
		return 1;
	}
	if (argc == 3) {
		fprintf(stderr, "hget: %lu bytes to %s\n", got, argv[2]);
	}
	return 0;
}
