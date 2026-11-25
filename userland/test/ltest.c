/* ltest PORT COUNT -- TCP listening (0.31), for tests/net_listen_test.py.
 * ltest -r -- two reads of /dev/random, 16 bytes each, in hex.
 *
 * Listens on PORT (and checks a second announce of it is refused), then
 * takes COUNT connections one after another: says who each is from,
 * sends back what it gets in capitals until the other end finishes
 * sending, then "bye N\n" (N: bytes it got) and closes. */
#include <ctype.h>
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "-r")) {
		int fd = open("/dev/random", OREAD);
		for (int k = 0; k < 2; k++) {
			unsigned char b[16];
			long n = fd < 0 ? -1 : read(fd, b, sizeof(b));
			printf("random %d:", k);
			for (long i = 0; i < n; i++) {
				printf(" %02x", b[i]);
			}
			printf(" (%ld)\n", n);
		}
		return 0;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: ltest PORT COUNT\n");
		return 2;
	}
	char addr[32];
	snprintf(addr, sizeof(addr), "tcp!*!%s", argv[1]);
	int l = announce(addr);
	if (l < 0) {
		printf("ltest: announce: %s\n", strerror(errno));
		return 1;
	}
	int again = announce(addr);
	printf("ltest: second announce: %s\n", again < 0 && errno == EADDRINUSE ? "in use" : "WRONG");
	if (again >= 0) {
		close(again);
	}
	if (accept_conn(0, NULL, 0) >= 0 || errno != EINVAL) {
		printf("ltest: accept on a non-listener: WRONG\n");
	}
	printf("ltest: listening on %s\n", argv[1]);
	static char buf[4096];
	for (int i = 0, n = atoi(argv[2]); i < n; i++) {
		char who[32];
		int c = accept_conn(l, who, sizeof(who));
		if (c < 0) {
			printf("ltest: accept: %s\n", strerror(errno));
			return 1;
		}
		printf("ltest: connection %d from %s\n", i + 1, who);
		long got, total = 0;
		while ((got = read(c, buf, sizeof(buf))) > 0) {
			total += got;
			for (long k = 0; k < got; k++) {
				buf[k] = (char)toupper((unsigned char)buf[k]);
			}
			for (long done = 0; done < got;) {
				long w = write(c, buf + done, (size_t)(got - done));
				if (w <= 0) {
					printf("ltest: write: %s\n", strerror(errno));
					return 1;
				}
				done += w;
			}
		}
		snprintf(buf, sizeof(buf), "bye %ld\n", total);
		write(c, buf, strlen(buf));
		close(c);
		printf("ltest: connection %d done, %ld bytes\n", i + 1, total);
	}
	close(l);
	printf("ltest: closed\n");
	return 0;
}
