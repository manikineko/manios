/* edit [FILE] -- a text editor for the terminal (0.30, a package).
 *
 * Full screen on the console or in a ManiDE terminal, with the keys a
 * terminal sends (con_raw): arrows, Home/End, PgUp/PgDn, Backspace and
 * Delete edit as you would expect, and the bottom line lists the rest:
 *
 *   ^O save   ^X exit   ^W find (again)   ^L go to line
 *   ^K cut the line (several in a row go together)   ^U paste them
 *
 * A file whose lines end in CR LF (DOS) is saved the same way. Tabs show
 * as spaces up to the next multiple of 8. The screen's size comes from
 * the terminal (ESC [ 18 t), 80x24 without an answer. Only the rows that
 * changed are redrawn, so it stays quick over a serial line. */
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAB 8
#define CTRL(c) ((c) & 0x1F)

enum {
	K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS, K_NONE
};

struct line {
	char *s;
	int len, cap;
};

static struct line *lines;
static int nlines, cap_lines;
static int cy, cx;            /* the cursor: line, byte in it */
static int want_col = -1;     /* the column up/down try to keep */
static int top, left;         /* the first line and column shown */
static int rows = 24, cols = 80;
static int modified, crlf;
static char filename[256];
static char message[160];
static char found[80];        /* the last search */
static struct line *clip;     /* cut lines */
static int nclip, cut_streak;
static char **shown;          /* what each screen row shows now */

/* --- output: one buffer per frame --- */

static char *out;
static size_t out_len, out_cap;

static void emit(const char *s, size_t n)
{
	if (out_len + n > out_cap) {
		out_cap = (out_len + n) * 2 + 1024;
		out = realloc(out, out_cap);
		if (!out) {
			exit(1);
		}
	}
	memcpy(out + out_len, s, n);
	out_len += n;
}

static void emits(const char *s)
{
	emit(s, strlen(s));
}

static void flush_out(void)
{
	size_t done = 0;
	while (done < out_len) {
		long n = write(1, out + done, out_len - done);
		if (n <= 0) {
			break;
		}
		done += (size_t)n;
	}
	out_len = 0;
}

/* --- the text --- */

static void line_reserve(struct line *l, int n)
{
	if (n + 1 > l->cap) {
		l->cap = n + 1 + l->cap / 2 + 16;
		l->s = realloc(l->s, (size_t)l->cap);
		if (!l->s) {
			exit(1);
		}
	}
}

static void insert_line(int at, const char *s, int len)
{
	if (nlines == cap_lines) {
		cap_lines = cap_lines ? cap_lines * 2 : 64;
		lines = realloc(lines, (size_t)cap_lines * sizeof(*lines));
		if (!lines) {
			exit(1);
		}
	}
	memmove(&lines[at + 1], &lines[at], (size_t)(nlines - at) * sizeof(*lines));
	struct line *l = &lines[at];
	memset(l, 0, sizeof(*l));
	line_reserve(l, len);
	memcpy(l->s, s, (size_t)len);
	l->len = len;
	l->s[len] = 0;
	nlines++;
}

static void delete_line(int at)
{
	free(lines[at].s);
	memmove(&lines[at], &lines[at + 1], (size_t)(nlines - at - 1) * sizeof(*lines));
	nlines--;
	if (!nlines) {
		insert_line(0, "", 0);
	}
}

static int load(const char *path)
{
	int fd = open(path, OREAD);
	if (fd < 0) {
		insert_line(0, "", 0);
		snprintf(message, sizeof(message), errno == ENOENT ? "New file" : "%s",
		         errno == ENOENT ? "" : strerror(errno));
		return errno == ENOENT ? 0 : -1;
	}
	size_t cap = 8192, len = 0;
	char *buf = malloc(cap);
	long n;
	while (buf && (n = read(fd, buf + len, cap - len)) > 0) {
		len += (size_t)n;
		if (len == cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
	}
	close(fd);
	if (!buf) {
		return -1;
	}
	size_t start = 0;
	int crs = 0, lfs = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i == len || buf[i] == '\n') {
			size_t end = i;
			if (i < len) {
				lfs++;
				if (end > start && buf[end - 1] == '\r') {
					end--;
					crs++;
				}
			}
			if (i < len || end > start) {
				insert_line(nlines, buf + start, (int)(end - start));
			}
			start = i + 1;
		}
	}
	if (!nlines) {
		insert_line(0, "", 0);
	}
	crlf = lfs && crs * 2 > lfs;
	free(buf);
	snprintf(message, sizeof(message), "%d line%s%s", nlines, nlines == 1 ? "" : "s",
	         crlf ? " (DOS line ends)" : "");
	return 0;
}

static int save(void)
{
	int fd = open(filename, OWRITE | O_CREAT | O_TRUNC);
	if (fd < 0) {
		snprintf(message, sizeof(message), "Cannot save %s: %s", filename, strerror(errno));
		return -1;
	}
	long total = 0;
	int ok = 1;
	for (int i = 0; i < nlines && ok; i++) {
		struct line *l = &lines[i];
		/* No newline after a last line that is empty: the file ended
		 * with one. */
		if (i == nlines - 1 && !l->len) {
			break;
		}
		ok = write(fd, l->s, (size_t)l->len) == l->len
		     && write(fd, crlf ? "\r\n" : "\n", crlf ? 2 : 1) == (crlf ? 2 : 1);
		total += l->len + (crlf ? 2 : 1);
	}
	close(fd);
	if (!ok) {
		snprintf(message, sizeof(message), "Writing %s failed: %s", filename, strerror(errno));
		return -1;
	}
	modified = 0;
	snprintf(message, sizeof(message), "Saved %s: %d lines, %ld bytes", filename, nlines, total);
	return 0;
}

/* --- columns: tabs take up to the next multiple of TAB --- */

static int col_of(const struct line *l, int x)
{
	int c = 0;
	for (int i = 0; i < x && i < l->len; i++) {
		c = l->s[i] == '\t' ? (c / TAB + 1) * TAB : c + 1;
	}
	return c;
}

static int x_of(const struct line *l, int col)
{
	int c = 0, i = 0;
	for (; i < l->len; i++) {
		int next = l->s[i] == '\t' ? (c / TAB + 1) * TAB : c + 1;
		if (next > col) {
			break;
		}
		c = next;
	}
	return i;
}

/* --- the screen --- */

static int text_rows(void)
{
	return rows - 3;
}

static void set_row(int r, const char *text)
{
	if (shown[r] && !strcmp(shown[r], text)) {
		return;
	}
	char pos[24];
	snprintf(pos, sizeof(pos), "\033[%d;1H", r + 1);
	emits(pos);
	emits(text);
	emits("\033[0m\033[K");
	free(shown[r]);
	shown[r] = strdup(text);
}

static void draw(void)
{
	int tr = text_rows();
	if (cy < top) {
		top = cy;
	}
	if (cy >= top + tr) {
		top = cy - tr + 1;
	}
	int ccol = col_of(&lines[cy], cx);
	if (ccol < left) {
		left = ccol;
	}
	if (ccol >= left + cols) {
		left = ccol - cols + 1;
	}
	char *buf = malloc((size_t)cols * 2 + 64);
	if (!buf) {
		exit(1);
	}
	/* The title. */
	int n = snprintf(buf, (size_t)cols * 2 + 64, "\033[30;46m edit  %s%s", filename[0] ? filename : "[no name]",
	                 modified ? "  [modified]" : "");
	char where[40];
	snprintf(where, sizeof(where), "line %d/%d col %d ", cy + 1, nlines, ccol + 1);
	int title_len = n - 8; /* without the colour escape */
	int pad = cols - title_len - (int)strlen(where);
	for (; pad > 0 && n < cols * 2; pad--) {
		buf[n++] = ' ';
	}
	snprintf(buf + n, (size_t)cols * 2 + 64 - (size_t)n, "%s", where);
	set_row(0, buf);
	/* The text, expanded tabs, from column left. */
	for (int r = 0; r < tr; r++) {
		int i = top + r;
		n = 0;
		if (i < nlines) {
			struct line *l = &lines[i];
			int c = 0;
			for (int k = 0; k < l->len && c < left + cols; k++) {
				int w = l->s[k] == '\t' ? (c / TAB + 1) * TAB - c : 1;
				for (int j = 0; j < w; j++, c++) {
					if (c >= left && c < left + cols) {
						unsigned char ch = (unsigned char)l->s[k];
						buf[n++] = ch == '\t' ? ' ' : (ch < 0x20 || ch == 0x7F) ? '?' : (char)ch;
					}
				}
			}
		} else {
			buf[n++] = '~';
		}
		buf[n] = 0;
		set_row(r + 1, buf);
	}
	/* The message, then the keys. */
	snprintf(buf, (size_t)cols * 2 + 64, "\033[1m%.*s", cols, message);
	set_row(rows - 2, buf);
	snprintf(buf, (size_t)cols * 2 + 64, "\033[7m%.*s", cols,
	         "^O Save  ^X Exit  ^W Find  ^L Line  ^K Cut  ^U Paste");
	set_row(rows - 1, buf);
	free(buf);
	char pos[24];
	snprintf(pos, sizeof(pos), "\033[%d;%dH", cy - top + 2, ccol - left + 1);
	emits(pos);
	flush_out();
}

static void redraw_all(void)
{
	for (int r = 0; r < rows; r++) {
		free(shown[r]);
		shown[r] = NULL;
	}
	emits("\033[0m\033[H\033[2J");
}

/* --- keys --- */

static int read_byte(int timeout_ms)
{
	if (timeout_ms >= 0) {
		struct zkt_pollfd pf = { 0, 0 };
		if (poll(&pf, 1, timeout_ms) <= 0 || !pf.ready) {
			return -1;
		}
	}
	unsigned char c;
	long n = read(0, &c, 1);
	return n == 1 ? c : -2;
}

/* A key: a byte, or K_* for an escape sequence; -2 at end of input. */
static int read_key(void)
{
	int c = read_byte(-1);
	if (c != 0x1B) {
		return c;
	}
	int a = read_byte(100);
	if (a < 0) {
		return 0x1B;
	}
	if (a != '[' && a != 'O') {
		return K_NONE;
	}
	int num = 0, b;
	for (;;) {
		b = read_byte(100);
		if (b < 0) {
			return K_NONE;
		}
		if (b >= '0' && b <= '9') {
			num = num * 10 + (b - '0');
			continue;
		}
		if (b == ';') {
			num = 0;
			continue;
		}
		break;
	}
	switch (b) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	case 'H': return K_HOME;
	case 'F': return K_END;
	case 't': return K_NONE; /* a late answer to the size question */
	case '~':
		switch (num) {
		case 1: case 7: return K_HOME;
		case 4: case 8: return K_END;
		case 2: return K_INS;
		case 3: return K_DEL;
		case 5: return K_PGUP;
		case 6: return K_PGDN;
		}
	}
	return K_NONE;
}

/* Asks the terminal its size: ESC [ 8 ; ROWS ; COLS t. */
static void ask_size(void)
{
	fputs("\033[18t", stdout);
	fflush(stdout);
	char buf[32];
	int n = 0, c;
	while (n < (int)sizeof(buf) - 1 && (c = read_byte(n ? 100 : 400)) >= 0) {
		buf[n++] = (char)c;
		if (c == 't') {
			break;
		}
	}
	buf[n] = 0;
	char *p = strstr(buf, "\033[8;"), *end;
	if (!p) {
		return;
	}
	long r = strtol(p + 4, &end, 10), k = *end == ';' ? strtol(end + 1, &end, 10) : 0;
	if (*end == 't' && r >= 6 && k >= 20) {
		rows = r > 200 ? 200 : (int)r;
		cols = k > 400 ? 400 : (int)k;
	}
}

/* A line of input on the message row; 0 if Esc or ^C. */
static int prompt(const char *question, char *answer, size_t size)
{
	size_t len = strlen(answer);
	for (;;) {
		snprintf(message, sizeof(message), "%s%s", question, answer);
		draw();
		char pos[24];
		snprintf(pos, sizeof(pos), "\033[%d;%dH", rows - 1, (int)(strlen(message) + 1));
		emits(pos);
		flush_out();
		int k = read_key();
		if (k == '\r' || k == '\n') {
			message[0] = 0;
			return 1;
		}
		if (k == 0x1B || k == CTRL('C') || k == -2) {
			message[0] = 0;
			return 0;
		}
		if ((k == 0x7F || k == '\b') && len) {
			answer[--len] = 0;
		} else if (k >= ' ' && k < 0x7F && len + 1 < size) {
			answer[len++] = (char)k;
			answer[len] = 0;
		}
	}
}

static void find(void)
{
	char q[sizeof(found)];
	snprintf(q, sizeof(q), "%s", found);
	if (!prompt("Find: ", q, sizeof(q)) || !q[0]) {
		return;
	}
	snprintf(found, sizeof(found), "%s", q);
	/* From just after the cursor, round the end to the start. */
	for (int step = 0; step <= nlines; step++) {
		int i = (cy + step) % nlines;
		int from = step == 0 ? cx + 1 : 0;
		if (from > lines[i].len) {
			continue;
		}
		char *hit = strstr(lines[i].s + from, found);
		if (hit) {
			int wrapped = cy + step >= nlines || (step == nlines);
			cy = i;
			cx = (int)(hit - lines[i].s);
			want_col = -1;
			snprintf(message, sizeof(message), "Found \"%s\"%s", found,
			         wrapped ? " (from the start)" : "");
			return;
		}
	}
	snprintf(message, sizeof(message), "\"%s\" is not in the text", found);
}

static void go_to_line(void)
{
	char q[16] = "";
	if (prompt("Go to line: ", q, sizeof(q)) && atoi(q) > 0) {
		cy = atoi(q) > nlines ? nlines - 1 : atoi(q) - 1;
		cx = 0;
		want_col = -1;
	}
}

static void cut_line(void)
{
	if (!cut_streak) {
		for (int i = 0; i < nclip; i++) {
			free(clip[i].s);
		}
		nclip = 0;
	}
	clip = realloc(clip, (size_t)(nclip + 1) * sizeof(*clip));
	if (!clip) {
		exit(1);
	}
	clip[nclip] = lines[cy];
	clip[nclip].s = strdup(lines[cy].s);
	nclip++;
	if (nlines == 1) {
		lines[0].len = 0;
		lines[0].s[0] = 0;
	} else {
		delete_line(cy);
		if (cy >= nlines) {
			cy = nlines - 1;
		}
	}
	cx = 0;
	modified = 1;
	snprintf(message, sizeof(message), "Cut %d line%s", nclip, nclip == 1 ? "" : "s");
}

static void paste(void)
{
	for (int i = 0; i < nclip; i++) {
		insert_line(cy + i, clip[i].s, clip[i].len);
	}
	if (nclip) {
		modified = 1;
		snprintf(message, sizeof(message), "Pasted %d line%s", nclip, nclip == 1 ? "" : "s");
	}
	cy += nclip;
	if (cy >= nlines) {
		cy = nlines - 1;
	}
	cx = 0;
}

static void insert_char(char c)
{
	struct line *l = &lines[cy];
	line_reserve(l, l->len + 1);
	memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
	l->s[cx++] = c;
	l->len++;
	modified = 1;
}

static void newline(void)
{
	struct line *l = &lines[cy];
	insert_line(cy + 1, l->s + cx, l->len - cx);
	l = &lines[cy];
	l->len = cx;
	l->s[cx] = 0;
	cy++;
	cx = 0;
	modified = 1;
}

/* Removes the byte before the cursor, joining lines at the start. */
static void backspace(void)
{
	if (cx > 0) {
		struct line *l = &lines[cy];
		memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
		l->len--;
		cx--;
		modified = 1;
	} else if (cy > 0) {
		struct line *prev = &lines[cy - 1];
		int at = prev->len;
		line_reserve(prev, prev->len + lines[cy].len);
		memcpy(prev->s + prev->len, lines[cy].s, (size_t)lines[cy].len + 1);
		prev->len += lines[cy].len;
		delete_line(cy);
		cy--;
		cx = at;
		modified = 1;
	}
}

static int quit_asks(void)
{
	if (!modified) {
		return 1;
	}
	char a[4] = "";
	if (!prompt("Save the changes first? (y = save, n = discard, Esc = stay) ", a, sizeof(a))) {
		return 0;
	}
	if (a[0] == 'y' || a[0] == 'Y') {
		if (!filename[0] && (!prompt("Save as: ", filename, sizeof(filename)) || !filename[0])) {
			return 0;
		}
		return save() == 0;
	}
	return a[0] == 'n' || a[0] == 'N';
}

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		fprintf(stderr, "usage: edit [FILE]\n");
		return 2;
	}
	if (argc == 2) {
		snprintf(filename, sizeof(filename), "%s", argv[1]);
		if (load(filename) < 0) {
			fprintf(stderr, "edit: %s: %s\n", filename, message);
			return 1;
		}
	} else {
		insert_line(0, "", 0);
	}
	struct zkt_dirent st;
	int fd = open("/dev/consctl", OREAD);
	if (fd < 0 && fstat(0, &st) == 0 && st.type == ZKT_TYPE_DEVICE) {
		fprintf(stderr, "edit: the console has no raw mode (/dev/consctl): "
		                "edit needs ManiOS 0.30.1 or later\n");
		return 1;
	}
	if (fd >= 0) {
		close(fd);
	}
	con_raw(1);
	ask_size();
	shown = calloc((size_t)rows, sizeof(*shown));
	if (!shown) {
		con_raw(0);
		return 1;
	}
	redraw_all();
	int done = 0;
	while (!done) {
		draw();
		int k = read_key();
		int cutting = 0;
		if (k != K_NONE) {
			message[0] = 0;
		}
		struct line *l = &lines[cy];
		switch (k) {
		case -2:
			done = 1; /* the input has gone */
			break;
		case CTRL('X'):
			done = quit_asks();
			break;
		case CTRL('O'):
			if (filename[0] || (prompt("Save as: ", filename, sizeof(filename)) && filename[0])) {
				save();
			}
			break;
		case CTRL('W'):
			find();
			break;
		case CTRL('L'):
			go_to_line();
			break;
		case CTRL('K'):
			cut_line();
			cutting = 1;
			break;
		case CTRL('U'):
			paste();
			break;
		case CTRL('R'):
			redraw_all();
			break;
		case '\r':
		case '\n':
			newline();
			break;
		case 0x7F:
		case '\b':
			backspace();
			break;
		case K_DEL:
			if (cx < l->len || cy + 1 < nlines) {
				if (cx < l->len) {
					cx++;
				} else {
					cy++;
					cx = 0;
				}
				backspace();
			}
			break;
		case K_LEFT:
			if (cx > 0) {
				cx--;
			} else if (cy > 0) {
				cy--;
				cx = lines[cy].len;
			}
			want_col = -1;
			break;
		case K_RIGHT:
			if (cx < l->len) {
				cx++;
			} else if (cy + 1 < nlines) {
				cy++;
				cx = 0;
			}
			want_col = -1;
			break;
		case K_UP:
		case K_DOWN:
		case K_PGUP:
		case K_PGDN: {
			if (want_col < 0) {
				want_col = col_of(l, cx);
			}
			int step = k == K_UP ? -1 : k == K_DOWN ? 1 : k == K_PGUP ? -text_rows() : text_rows();
			cy += step;
			cy = cy < 0 ? 0 : cy >= nlines ? nlines - 1 : cy;
			cx = x_of(&lines[cy], want_col);
			break;
		}
		case K_HOME:
			cx = 0;
			want_col = -1;
			break;
		case K_END:
			cx = l->len;
			want_col = -1;
			break;
		case '\t':
			insert_char('\t');
			break;
		default:
			if (k >= ' ' && k < 0x100 && k != 0x7F) {
				insert_char((char)k);
				want_col = -1;
			}
		}
		cut_streak = cutting;
	}
	emits("\033[0m\033[H\033[2J");
	flush_out();
	con_raw(0);
	return 0;
}
