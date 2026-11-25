/* con_raw: keys one at a time (0.30). On the console, through
 * /dev/consctl (Plan 9's "rawon"); in a ManiDE terminal, whose programs
 * read a pipe, through the escape ESC [ ? 7727 h that term understands
 * (other terminals ignore it). Either way arrows and editing keys come
 * as ANSI sequences: ESC [ A..D, ESC [ H / F, ESC [ 2~ 3~ 5~ 6~. */
#include <manios.h>
#include <stdio.h>

static int ctl = -1;

int con_raw(int on)
{
	struct zkt_dirent st;
	fputs(on ? "\033[?7727h" : "\033[?7727l", stdout);
	fflush(stdout);
	if (on) {
		if (ctl < 0 && fstat(0, &st) == 0 && st.type == ZKT_TYPE_DEVICE) {
			ctl = open("/dev/consctl", OWRITE);
			if (ctl >= 0 && write(ctl, "rawon", 5) != 5) {
				close(ctl);
				ctl = -1;
			}
		}
	} else if (ctl >= 0) {
		write(ctl, "rawoff", 6);
		close(ctl);
		ctl = -1;
	}
	return 0;
}
