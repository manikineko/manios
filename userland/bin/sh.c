/* sh -- the ManiOS shell.
 *
 *   sh              read commands from standard input, with a prompt
 *   sh FILE         run the commands in FILE
 *   sh -c COMMAND   run one command line
 *
 * Commands are separated by newlines or ';'; words by blanks. '...'
 * quotes literally, "..." quotes with \" and \\ escapes, \ escapes the
 * next character, and # at the start of a word begins a comment.
 * Pipelines join commands with '|'; '< FILE' redirects standard input,
 * '> FILE' standard output (made, or emptied, 0.31) and '>> FILE' adds
 * to the end of one. A command ending in '&' runs in the background
 * (0.31): the shell says its pid, goes on, and says when it is done;
 * `jobs` lists them, `kill PID` stops one. On the console ^C stops the
 * command the shell waits for. A name without '/' runs /bin/NAME; the
 * program shares the shell's namespace, so `bind` and `unbind` change
 * the shell's view too (ADR-0003). Builtins: cd, exit, newns, jobs,
 * kill, help. */
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 512
#define WORDS_MAX 48
#define STAGES_MAX 8
#define PROMPT "manios% "

static int status; /* of the last command: an exit code, or 128 + the vector that killed it */

/* Operators are these very strings (compared by address), so a quoted
 * "|" stays an ordinary word. */
static char OP_PIPE[] = "|", OP_IN[] = "<", OP_OUT[] = ">", OP_APPEND[] = ">>";

/* Background jobs (0.31): the last process of each, to say when it ends. */
#define JOBS_MAX 16
static struct job {
	int pid;
	char name[40];
} jobs[JOBS_MAX];

static int consctl = -1; /* the console's control file, when it is ours: ^C */

static int blank(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int special(char c)
{
	return c == ';' || c == '|' || c == '<' || c == '>' || c == '&';
}

/* Splits the first command line of *line into words and operators,
 * copying the words into buf (at least twice the line's length: every
 * word gains a NUL), and points *line at what follows the ';' (NULL at
 * the end). Returns the count, or -1 after reporting a syntax error. */
static int split(char **line, char **words, int max, char *buf, int *bg)
{
	char *in = *line, *out = buf;
	int n = 0;
	*line = NULL;
	*bg = 0;
	for (;;) {
		while (blank(*in)) {
			in++;
		}
		if (!*in || *in == '#') {
			break;
		}
		if (*in == ';' || *in == '&') {
			*bg = *in == '&';
			*line = in + 1;
			break;
		}
		if (n == max) {
			fprintf(stderr, "sh: too many words\n");
			return -1;
		}
		if (*in == '>' && in[1] == '>') {
			words[n++] = OP_APPEND;
			in += 2;
			continue;
		}
		if (*in == '|' || *in == '<' || *in == '>') {
			words[n++] = *in == '|' ? OP_PIPE : *in == '<' ? OP_IN : OP_OUT;
			in++;
			continue;
		}
		words[n++] = out;
		while (*in && !blank(*in) && !special(*in)) {
			if (*in == '\'' || *in == '"') {
				char quote = *in++;
				while (*in && *in != quote) {
					if (quote == '"' && *in == '\\' && (in[1] == '"' || in[1] == '\\')) {
						in++;
					}
					*out++ = *in++;
				}
				if (!*in) {
					fprintf(stderr, "sh: missing %c\n", quote);
					return -1;
				}
				in++;
			} else if (*in == '\\' && in[1]) {
				in++;
				*out++ = *in++;
			} else {
				*out++ = *in++;
			}
		}
		*out++ = '\0';
	}
	words[n] = NULL;
	return n;
}

static void builtin_help(void)
{
	printf("builtins: cd [DIR], exit [CODE], newns, jobs, kill PID..., help\n"
	       "other commands run from /bin (ls /bin), or by path;\n"
	       "join them with |, redirect with < FILE, > FILE and >> FILE;\n"
	       "COMMAND & runs it in the background (kill PID stops it)\n");
}

/* Returns 1 if argv[0] was a builtin (and ran it). */
static int builtin(int argc, char **argv)
{
	if (!strcmp(argv[0], "cd")) {
		const char *dir = argc > 1 ? argv[1] : "/";
		status = 0;
		if (chdir(dir) < 0) {
			fprintf(stderr, "sh: cd: %s: %s\n", dir, strerror(errno));
			status = 1;
		}
	} else if (!strcmp(argv[0], "exit")) {
		exit(argc > 1 ? atoi(argv[1]) : status);
	} else if (!strcmp(argv[0], "newns")) {
		/* From now on, binds made here (and by commands run from here)
		 * stay private to this shell and its children. */
		status = nsfork() < 0;
		if (status) {
			perror("sh: newns");
		}
	} else if (!strcmp(argv[0], "help")) {
		builtin_help();
		status = 0;
	} else if (!strcmp(argv[0], "kill")) {
		/* 0.31: stops processes (`cat /dev/ps` lists them). A builtin:
		 * the boot archive has no room to spare. -SIGNAL is accepted and
		 * means the same: ManiOS has no signals. */
		int a = argc > 1 && argv[1][0] == '-' ? 2 : 1;
		status = a >= argc;
		if (status) {
			fprintf(stderr, "usage: kill [-SIGNAL] PID...\n");
		}
		for (; a < argc; a++) {
			char *end;
			long pid = strtol(argv[a], &end, 10);
			if (*end || pid <= 0) {
				fprintf(stderr, "kill: %s: not a process number\n", argv[a]);
				status = 1;
			} else if (proc_kill((int)pid) < 0) {
				fprintf(stderr, "kill: %ld: %s\n", pid, strerror(errno));
				status = 1;
			}
		}
	} else if (!strcmp(argv[0], "jobs")) {
		for (int i = 0; i < JOBS_MAX; i++) {
			if (jobs[i].pid) {
				printf("[%d] running  %s\n", jobs[i].pid, jobs[i].name);
			}
		}
		status = 0;
	} else {
		return 0;
	}
	return 1;
}

static int builtin_name(const char *name)
{
	return !strcmp(name, "cd") || !strcmp(name, "exit") || !strcmp(name, "newns")
	       || !strcmp(name, "help") || !strcmp(name, "jobs") || !strcmp(name, "kill");
}

struct stage {
	char **argv;
	int argc;
	const char *in, *out; /* redirections, or NULL */
	int append;           /* out is >>: added to */
};

/* Splits the words into pipeline stages; -1 after a syntax error. */
static int parse(char **words, int n, struct stage *stages)
{
	int count = 0;
	struct stage *s = &stages[0];
	memset(s, 0, sizeof(*s));
	s->argv = words;
	for (int i = 0; i <= n; i++) {
		char *w = words[i];
		if (w == OP_IN || w == OP_OUT || w == OP_APPEND) {
			char *file = i + 1 < n ? words[i + 1] : NULL;
			if (!file || file == OP_PIPE || file == OP_IN || file == OP_OUT || file == OP_APPEND) {
				fprintf(stderr, "sh: %s needs a file\n", w);
				return -1;
			}
			*(w == OP_IN ? &s->in : &s->out) = file;
			if (w != OP_IN) {
				s->append = w == OP_APPEND;
			}
			words[i] = words[i + 1] = NULL;
			i++;
			continue;
		}
		if (w && w != OP_PIPE) {
			s->argv[s->argc++] = w; /* compacts over the removed redirections */
			continue;
		}
		/* The end of a stage: '|' or the end of the words. */
		if (!s->argc) {
			fprintf(stderr, "sh: empty command in a pipeline\n");
			return -1;
		}
		s->argv[s->argc] = NULL;
		count++;
		if (!w) {
			break;
		}
		if (count == STAGES_MAX) {
			fprintf(stderr, "sh: pipeline too long\n");
			return -1;
		}
		s = &stages[count];
		memset(s, 0, sizeof(*s));
		s->argv = &words[i + 1];
	}
	return count;
}

static void report(const char *name, int st)
{
	if ((st & ZKT_WAIT_KILLED) && ZKT_WAIT_VECTOR(st) == ZKT_KILL_VECTOR) {
		status = 130; /* kill or ^C: said already, or asked for */
	} else if (st & ZKT_WAIT_KILLED) {
		fprintf(stderr, "sh: %s: killed (vector %d)\n", name, ZKT_WAIT_VECTOR(st));
		status = 128 + ZKT_WAIT_VECTOR(st);
	} else {
		status = ZKT_WAIT_CODE(st);
	}
}

/* Runs the stages with pipes between them. Each child is spawned with
 * the shell's own standard input and output pointed where it needs
 * them (children inherit descriptors 0-2), which are then put back. */
/* Tells the console which processes ^C stops ("fg" alone: none). */
static void foreground(const int *pids, int count)
{
	if (consctl < 0) {
		return;
	}
	char msg[96] = "fg";
	size_t n = 2;
	for (int k = 0; k < count; k++) {
		if (pids[k] > 0) {
			n += (size_t)snprintf(msg + n, sizeof(msg) - n, " %d", pids[k]);
		}
	}
	write(consctl, msg, n);
}

/* Background jobs that have ended: said before the next prompt. */
static void reap_jobs(void)
{
	int pid, st;
	while ((pid = reap(&st)) > 0) {
		for (int i = 0; i < JOBS_MAX; i++) {
			if (jobs[i].pid == pid) {
				if ((st & ZKT_WAIT_KILLED) && ZKT_WAIT_VECTOR(st) == ZKT_KILL_VECTOR) {
					printf("[%d] killed  %s\n", pid, jobs[i].name);
				} else if (st & ZKT_WAIT_KILLED) {
					printf("[%d] killed (vector %d)  %s\n", pid, ZKT_WAIT_VECTOR(st), jobs[i].name);
				} else {
					printf("[%d] done (%d)  %s\n", pid, ZKT_WAIT_CODE(st), jobs[i].name);
				}
				jobs[i].pid = 0;
			}
		}
	}
}

static void run_pipeline(struct stage *stages, int count, int bg)
{
	int saved_in = dup(0), saved_out = dup(1);
	int pids[STAGES_MAX], next_in = -1;
	fflush(stdout);
	for (int k = 0; k < count; k++) {
		struct stage *s = &stages[k];
		int in = next_in, out = -1, link[2] = { -1, -1 };
		next_in = -1;
		if (s->in) {
			if (in >= 0) {
				close(in);
			}
			if ((in = open(s->in, OREAD)) < 0) {
				fprintf(stderr, "sh: %s: %s\n", s->in, strerror(errno));
			}
		}
		if (s->out) {
			/* A device (/dev/...) is written as it is; a file is made,
			 * emptied (>), or added to (>>). */
			int how = OWRITE | O_CREAT | (s->append ? 0 : O_TRUNC);
			if ((out = open(s->out, how)) < 0 && errno == EINVAL) {
				out = open(s->out, OWRITE);
			}
			if (out < 0) {
				fprintf(stderr, "sh: %s: %s\n", s->out, strerror(errno));
			} else if (s->append) {
				lseek(out, 0, SEEK_END);
			}
		} else if (k < count - 1 && pipe(link) == 0) {
			out = link[0]; /* this stage writes one end, the next reads the other */
			next_in = link[1];
		}
		int pid = -1;
		if ((!s->in || in >= 0) && (!s->out || out >= 0) && !builtin_name(s->argv[0])) {
			char path[ZKT_PATH_MAX + 1];
			if (strchr(s->argv[0], '/')) {
				strlcpy(path, s->argv[0], sizeof(path));
			} else {
				snprintf(path, sizeof(path), "/bin/%s", s->argv[0]);
			}
			dup2(in >= 0 ? in : saved_in, 0);
			dup2(out >= 0 ? out : saved_out, 1);
			pid = spawn(path, s->argv);
			dup2(saved_in, 0);
			dup2(saved_out, 1);
			if (pid < 0) {
				fprintf(stderr, "sh: %s: %s\n", s->argv[0],
				        errno == ENOENT ? "not found" : strerror(errno));
			}
		} else if (builtin_name(s->argv[0])) {
			fprintf(stderr, "sh: %s: a builtin can't be in a pipeline or redirected\n", s->argv[0]);
		}
		/* The shell's copies go, or readers would never see end of file. */
		if (in >= 0) {
			close(in);
		}
		if (out >= 0) {
			close(out);
		}
		pids[k] = pid;
	}
	if (next_in >= 0) {
		close(next_in);
	}
	if (bg) {
		int last = pids[count - 1];
		status = last > 0 ? 0 : 127;
		for (int i = 0; i < JOBS_MAX && last > 0; i++) {
			if (!jobs[i].pid) {
				jobs[i].pid = last;
				snprintf(jobs[i].name, sizeof(jobs[i].name), "%s", stages[0].argv[0]);
				break;
			}
		}
		if (last > 0) {
			printf("[%d]\n", last);
		}
		close(saved_in);
		close(saved_out);
		return;
	}
	foreground(pids, count);
	status = 127;
	for (int k = 0; k < count; k++) {
		int st;
		if (pids[k] > 0 && wait(pids[k], &st) == pids[k]) {
			report(stages[k].argv[0], st);
		}
	}
	foreground(NULL, 0);
	if (pids[count - 1] <= 0) {
		status = 127;
	}
	close(saved_in);
	close(saved_out);
}

static void run(char *line)
{
	while (line) {
		static char buf[2 * LINE_MAX];
		char *words[WORDS_MAX + 1];
		struct stage stages[STAGES_MAX];
		int bg;
		int n = split(&line, words, WORDS_MAX, buf, &bg);
		int count = n > 0 ? parse(words, n, stages) : n;
		if (count < 0) {
			status = 2;
			return;
		}
		if (count == 1 && !bg && !stages[0].in && !stages[0].out && builtin(stages[0].argc, stages[0].argv)) {
			continue;
		}
		if (count > 0) {
			run_pipeline(stages, count, bg);
		}
	}
}

int main(int argc, char **argv)
{
	char line[LINE_MAX];
	if (argc == 3 && !strcmp(argv[1], "-c")) {
		strlcpy(line, argv[2], sizeof(line));
		run(line);
		return status;
	}
	if (argc > 2) {
		fprintf(stderr, "usage: sh [FILE | -c COMMAND]\n");
		return 2;
	}
	FILE *in = stdin;
	if (argc == 2 && !(in = fopen(argv[1], "r"))) {
		fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
		return 127;
	}
	struct zkt_dirent self;
	if (in == stdin && fstat(0, &self) == 0 && self.type == ZKT_TYPE_DEVICE && !strcmp(self.name, "cons")) {
		consctl = open("/dev/consctl", OWRITE);
	}
	for (;;) {
		reap_jobs();
		if (in == stdin) {
			fputs(PROMPT, stdout);
		}
		if (!fgets(line, sizeof(line), in)) {
			break;
		}
		run(line);
	}
	if (in == stdin) {
		putchar('\n'); /* end of input left the cursor after the prompt */
	}
	return status;
}
