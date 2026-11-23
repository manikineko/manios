/* pkg -- install software from the ManiOS repository (0.29.0).
 *
 *   pkg update               fetch the repository index
 *   pkg list [-i]            what the repository has (-i: what is installed)
 *   pkg search WORD          packages whose name or description has WORD
 *   pkg info NAME            one package in detail
 *   pkg install NAME...      download, check and install
 *   pkg remove NAME...       uninstall
 *   pkg upgrade              install newer versions of installed packages
 *
 * Options, before the command: -r ROOT (where packages go; default the
 * first writable disk's /n/DISK/manios/pkg), -u URL (the repository;
 * default http://manios.nl/repo, or the one the last update used).
 *
 * Trust. ManiOS has no TLS, so the repository is fetched over plain HTTP
 * and nothing is believed because of where it came from: the index
 * (index.txt) must carry a valid Ed25519 signature (index.sig) by the
 * key built in below, and every package must match the size and SHA-256
 * the signed index gives it. A package is a ustar archive. Native
 * packages hold bin/ and share/NAME/ files; DOS packages hold a DOS
 * program's files, which go to DOS/NAME, and their commands become
 * launchers in BIN that run them through dosrun. The kernel binds
 * ROOT/bin after /bin at boot; pkg binds it at once after an install.
 *
 * Names on disk are 8.3 (ManiOS writes FAT 8.3 names): package names are
 * at most 8 characters, and the repository builder checks every file. */
#include <ed25519.h>
#include <errno.h>
#include <http.h>
#include <manios.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define DEFAULT_URL "http://manios.nl/repo"
#define INDEX_MAX (512 * 1024)
#define CMDS_MAX 8

/* The repository signing key (repo-signing/manios-repo-ed25519.pub). */
static const char PUBLIC_KEY_HEX[] =
	"dd9cb68b51b10deb64aa80e455a711ad88ba6a17dd84fc561c3b9091fcf06020";

struct pkg {
	char name[9], version[32], kind[8], file[96], sha256[65], license[64];
	char title[96], summary[200], status[200];
	long size;
	int ncmds;
	char cmd[CMDS_MAX][9], target[CMDS_MAX][64];
	int nenv;
	char env[4][80]; /* launcher lines: "env NAME=VALUE" or "envpath NAME=DIR" */
};

static const char *repo = DEFAULT_URL;
static char saved_repo[200];
static int repo_given;
static char root[128];
static struct pkg *pkgs;
static int npkgs;

static void die(const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "pkg: ");
	fprintf(stderr, fmt, a ? a : "", b ? b : "");
	fprintf(stderr, "\n");
	exit(1);
}

/* The type of what path names (ZKT_TYPE_*), or -1 if nothing. */
static int type_of(const char *path)
{
	int fd = open(path, OREAD);
	if (fd < 0) {
		return -1;
	}
	struct zkt_dirent d;
	int rc = fstat(fd, &d);
	close(fd);
	return rc < 0 ? -1 : (int)d.type;
}

static int is_dir(const char *path)
{
	return type_of(path) == ZKT_TYPE_DIR;
}

static int exists(const char *path)
{
	return type_of(path) >= 0;
}

static void path_of(char *out, size_t n, const char *a, const char *b)
{
	snprintf(out, n, "%s/%s", a, b);
}

/* mkdir -p for a path below root. */
static int make_dirs(const char *path)
{
	char p[256];
	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			if (!is_dir(p) && mkdir(p, 0755) < 0 && errno != EEXIST) {
				return -1;
			}
			*s = '/';
		}
	}
	if (!is_dir(p) && mkdir(p, 0755) < 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

/* The package root: -r, else an existing /n/DISK/manios/pkg, else the
 * first disk where one can be made. */
static void find_root(void)
{
	if (root[0]) {
		if (make_dirs(root) < 0) {
			die("%s: cannot create: %s", root, strerror(errno));
		}
		return;
	}
	DIR *d = opendir("/n");
	if (!d) {
		die("no disks under /n%s%s", 0, 0);
	}
	char candidate[128] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char p[128];
		snprintf(p, sizeof(p), "/n/%s/manios/pkg", e->d_name);
		if (is_dir(p)) {
			snprintf(root, sizeof(root), "%s", p);
			closedir(d);
			return;
		}
		if (!candidate[0]) {
			char m[128];
			snprintf(m, sizeof(m), "/n/%s/manios", e->d_name);
			if ((is_dir(m) || mkdir(m, 0755) == 0) && (mkdir(p, 0755) == 0 || is_dir(p))) {
				snprintf(candidate, sizeof(candidate), "%s", p);
			}
		}
	}
	closedir(d);
	if (!candidate[0]) {
		die("no writable FAT disk for packages (attach one, or use -r DIR)%s%s", 0, 0);
	}
	snprintf(root, sizeof(root), "%s", candidate);
}

static void root_dirs(void)
{
	static const char *sub[] = {"bin", "dos", "db", "cache"};
	for (unsigned i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
		char p[160];
		path_of(p, sizeof(p), root, sub[i]);
		if (make_dirs(p) < 0) {
			die("%s: %s", p, strerror(errno));
		}
	}
}

/* --- files --- */

static long read_file(const char *path, char *buf, long max)
{
	int fd = open(path, OREAD);
	if (fd < 0) {
		return -1;
	}
	long n = 0, r;
	while (n < max && (r = read(fd, buf + n, (size_t)(max - n))) > 0) {
		n += r;
	}
	close(fd);
	return n;
}

static int write_file(const char *path, const void *data, size_t len)
{
	int fd = open(path, OWRITE | O_CREAT | O_TRUNC);
	if (fd < 0) {
		return -1;
	}
	const char *p = data;
	while (len) {
		long n = write(fd, p, len);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return close(fd);
}

/* --- fetching --- */

struct membuf {
	char *data;
	size_t len, max;
};

static int to_mem(void *ctx, const void *data, size_t len)
{
	struct membuf *m = ctx;
	if (m->len + len > m->max) {
		return 1;
	}
	memcpy(m->data + m->len, data, len);
	m->len += len;
	return 0;
}

static long fetch_mem(const char *name, char *buf, size_t max)
{
	char url[256];
	snprintf(url, sizeof(url), "%s/%s", repo, name);
	struct membuf m = {buf, 0, max};
	struct http_result res;
	if (http_get(url, to_mem, &m, &res)) {
		die("%s: %s", url, res.error);
	}
	if (res.status != 200) {
		char s[16];
		snprintf(s, sizeof(s), "%d", res.status);
		die("%s: HTTP %s", url, s);
	}
	return (long)m.len;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
	       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int unhex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
		if (b < 0) {
			return -1;
		}
		out[i] = (uint8_t)(a << 4 | b);
	}
	return 0;
}

static int signature_ok(const char *index, long len, const char *sighex)
{
	uint8_t pk[32], sig[64];
	unhex(PUBLIC_KEY_HEX, pk, 32);
	if (strlen(sighex) < 128 || unhex(sighex, sig, 64) < 0) {
		return 0;
	}
	return ed25519_verify(sig, index, (size_t)len, pk) == 0;
}

/* --- the index --- */

static void parse_index(char *text)
{
	free(pkgs);
	pkgs = 0;
	npkgs = 0;
	int cap = 0;
	struct pkg *cur = 0;
	char *line = text;
	if (strncmp(text, "manios-repo 1\n", 14) != 0) {
		die("the index is not a version-1 ManiOS repository index%s%s", 0, 0);
	}
	while (line && *line) {
		char *end = strchr(line, '\n');
		if (end) {
			*end = 0;
		}
		char *sp = strchr(line, ' ');
		const char *val = sp ? sp + 1 : "";
		if (sp) {
			*sp = 0;
		}
		if (!strcmp(line, "pkg")) {
			if (npkgs == cap) {
				cap = cap ? cap * 2 : 64;
				pkgs = realloc(pkgs, (size_t)cap * sizeof(*pkgs));
				if (!pkgs) {
					die("out of memory%s%s", 0, 0);
				}
			}
			cur = &pkgs[npkgs++];
			memset(cur, 0, sizeof(*cur));
			snprintf(cur->name, sizeof(cur->name), "%s", val);
		} else if (cur && !strcmp(line, "end")) {
			cur = 0;
		} else if (cur) {
#define FIELD(key, dst) else if (!strcmp(line, key)) snprintf(dst, sizeof(dst), "%s", val)
			if (0) {
			}
			FIELD("version", cur->version);
			FIELD("kind", cur->kind);
			FIELD("file", cur->file);
			FIELD("sha256", cur->sha256);
			FIELD("license", cur->license);
			FIELD("title", cur->title);
			FIELD("summary", cur->summary);
			FIELD("status", cur->status);
			else if (!strcmp(line, "size")) {
				cur->size = atol(val);
			} else if ((!strcmp(line, "env") || !strcmp(line, "envpath")) && cur->nenv < 4
			           && strchr(val, '=') && !strstr(val, "..")) {
				snprintf(cur->env[cur->nenv++], 80, "%s %s", line, val);
			} else if (!strcmp(line, "cmd") && cur->ncmds < CMDS_MAX) {
				const char *eq = strchr(val, '=');
				if (eq && eq - val <= 8) {
					snprintf(cur->cmd[cur->ncmds], 9, "%.*s", (int)(eq - val), val);
					snprintf(cur->target[cur->ncmds], 64, "%s", eq + 1);
					cur->ncmds++;
				}
			}
#undef FIELD
		}
		line = end ? end + 1 : 0;
	}
}

static void update(void)
{
	char *index = malloc(INDEX_MAX + 1), sig[256];
	if (!index) {
		die("out of memory%s%s", 0, 0);
	}
	long n = fetch_mem("index.txt", index, INDEX_MAX);
	long s = fetch_mem("index.sig", sig, sizeof(sig) - 1);
	sig[s] = 0;
	if (!signature_ok(index, n, sig)) {
		die("the repository index has no valid signature: not using it%s%s", 0, 0);
	}
	char p[160];
	path_of(p, sizeof(p), root, "index.txt");
	if (write_file(p, index, (size_t)n) < 0) {
		die("%s: %s", p, strerror(errno));
	}
	path_of(p, sizeof(p), root, "index.sig");
	if (write_file(p, sig, (size_t)s) < 0) {
		die("%s: %s", p, strerror(errno));
	}
	path_of(p, sizeof(p), root, "repo.url");
	write_file(p, repo, strlen(repo)); /* later commands use the same repository */
	index[n] = 0;
	parse_index(index);
	printf("pkg: %d packages available from %s\n", npkgs, repo);
	free(index);
}

/* The saved index, checked again; fetched if there is none. */
static void load_index(void)
{
	char *index = malloc(INDEX_MAX + 1), sig[256], p[160];
	if (!index) {
		die("out of memory%s%s", 0, 0);
	}
	path_of(p, sizeof(p), root, "index.txt");
	long n = read_file(p, index, INDEX_MAX);
	path_of(p, sizeof(p), root, "index.sig");
	long s = read_file(p, sig, sizeof(sig) - 1);
	if (n <= 0 || s <= 0) {
		free(index);
		update();
		return;
	}
	sig[s] = 0;
	if (!signature_ok(index, n, sig)) {
		free(index);
		fprintf(stderr, "pkg: the saved index fails its signature check; fetching it again\n");
		update();
		return;
	}
	index[n] = 0;
	parse_index(index);
	free(index);
}

static struct pkg *find(const char *name)
{
	for (int i = 0; i < npkgs; i++) {
		if (!strcmp(pkgs[i].name, name)) {
			return &pkgs[i];
		}
	}
	return 0;
}

/* The installed version, or "" (from DB/NAME.LST's first line). */
static void installed_version(const char *name, char *out, size_t n)
{
	char p[160], buf[64];
	snprintf(p, sizeof(p), "%s/db/%s.lst", root, name);
	long r = read_file(p, buf, sizeof(buf) - 1);
	out[0] = 0;
	if (r > 0) {
		buf[r] = 0;
		char *sp = strchr(buf, ' '), *nl = strchr(buf, '\n');
		if (sp && nl && nl > sp) {
			*nl = 0;
			snprintf(out, n, "%s", sp + 1);
		}
	}
}

/* --- installing --- */

struct download {
	int fd;
	struct sha256 h;
	long got;
};

static int to_file(void *ctx, const void *data, size_t len)
{
	struct download *d = ctx;
	sha256_update(&d->h, data, len);
	d->got += (long)len;
	const char *p = data;
	while (len) {
		long n = write(d->fd, p, len);
		if (n <= 0) {
			return 1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* One path component: 8.3, letters, digits and - _ ~ $ ! # % & only. */
static int component_ok(const char *s, size_t n)
{
	size_t base = 0, ext = 0;
	int dot = 0;
	if (!n) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (c == '.') {
			if (dot || i == 0) {
				return 0;
			}
			dot = 1;
			continue;
		}
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		      || strchr("-_~$!#%&", c))) {
			return 0;
		}
		if (dot) {
			ext++;
		} else {
			base++;
		}
	}
	return base >= 1 && base <= 8 && ext <= 3 && !(dot && !ext);
}

static int path_ok(const char *p)
{
	if (!*p || *p == '/') {
		return 0;
	}
	while (*p) {
		const char *e = strchr(p, '/');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (!component_ok(p, n)) {
			return 0;
		}
		p += n;
		if (*p == '/') {
			p++;
			if (!*p) {
				return 1; /* a directory entry's trailing slash */
			}
		}
	}
	return 1;
}

static unsigned long octal(const char *s, size_t n)
{
	unsigned long v = 0;
	for (size_t i = 0; i < n && s[i]; i++) {
		if (s[i] >= '0' && s[i] <= '7') {
			v = v * 8 + (unsigned long)(s[i] - '0');
		}
	}
	return v;
}

/* Extracts the archive into ROOT (native: bin/ and share/NAME/ only) or
 * ROOT/dos/NAME (dos), appending each file installed to `list`. */
static int extract(const struct pkg *k, const char *tar, FILE *list)
{
	int fd = open(tar, OREAD);
	if (fd < 0) {
		return -1;
	}
	int dos = !strcmp(k->kind, "dos");
	char dest_base[160];
	if (dos) {
		snprintf(dest_base, sizeof(dest_base), "%s/dos/%s", root, k->name);
	} else {
		snprintf(dest_base, sizeof(dest_base), "%s", root);
	}
	uint8_t h[512];
	static uint8_t buf[8192];
	int rc = 0;
	for (;;) {
		long n = read(fd, h, 512);
		if (n != 512) {
			rc = n == 0 ? 0 : -1;
			break;
		}
		int zero = 1;
		for (int i = 0; i < 512 && zero; i++) {
			zero = !h[i];
		}
		if (zero) {
			break;
		}
		unsigned long sum = 0;
		for (int i = 0; i < 512; i++) {
			sum += (i >= 148 && i < 156) ? ' ' : h[i];
		}
		if (sum != octal((char *)h + 148, 8) || memcmp(h + 257, "ustar", 5)) {
			fprintf(stderr, "pkg: %s: not a valid ustar archive\n", k->name);
			rc = -1;
			break;
		}
		char name[256];
		if (h[345]) {
			snprintf(name, sizeof(name), "%.155s/%.100s", (char *)h + 345, (char *)h);
		} else {
			snprintf(name, sizeof(name), "%.100s", (char *)h);
		}
		char type = (char)h[156];
		unsigned long size = octal((char *)h + 124, 12);
		int allowed = path_ok(name);
		if (allowed && !dos) {
			char share[32];
			snprintf(share, sizeof(share), "share/%s/", k->name);
			allowed = !strncmp(name, "bin/", 4) || !strncmp(name, share, strlen(share))
			          || !strcmp(name, "bin/") || !strncmp(name, "share/", 6);
			if (!strncmp(name, "share/", 6) && strncmp(name, share, strlen(share))
			    && strcmp(name, "share/")) {
				allowed = 0;
			}
		}
		if (!allowed || (type != '0' && type != 0 && type != '5')) {
			fprintf(stderr, "pkg: %s: refusing archive entry %s\n", k->name, name);
			rc = -1;
			break;
		}
		char dest[256];
		snprintf(dest, sizeof(dest), "%s/%s", dest_base, name);
		size_t dl = strlen(dest);
		if (dl && dest[dl - 1] == '/') {
			dest[dl - 1] = 0;
		}
		if (type == '5') {
			if (make_dirs(dest) < 0) {
				rc = -1;
				break;
			}
			continue;
		}
		char *slash = strrchr(dest, '/');
		*slash = 0;
		if (make_dirs(dest) < 0) {
			rc = -1;
			break;
		}
		*slash = '/';
		if (!dos && exists(dest)) {
			char owner[16];
			installed_version(k->name, owner, sizeof(owner));
			if (!owner[0]) {
				fprintf(stderr, "pkg: %s: %s already exists\n", k->name, dest);
				rc = -1;
				break;
			}
		}
		int out = open(dest, OWRITE | O_CREAT | O_TRUNC);
		if (out < 0) {
			fprintf(stderr, "pkg: %s: %s\n", dest, strerror(errno));
			rc = -1;
			break;
		}
		fprintf(list, "%s\n", dest + strlen(root) + 1);
		unsigned long left = size;
		while (left && rc == 0) {
			size_t want = left > sizeof(buf) ? sizeof(buf) : left;
			long r = read(fd, buf, want);
			if (r <= 0 || write(out, buf, (size_t)r) != r) {
				rc = -1;
			}
			left -= r > 0 ? (unsigned long)r : left;
		}
		close(out);
		unsigned long pad = (512 - size % 512) % 512;
		if (rc == 0 && pad && read(fd, buf, pad) != (long)pad) {
			rc = -1;
		}
		if (rc) {
			break;
		}
	}
	close(fd);
	return rc;
}

static void bind_bin(void)
{
	char p[160];
	path_of(p, sizeof(p), root, "bin");
	/* Bound already when one of its commands is visible in /bin. */
	DIR *d = opendir(p);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char q[160], r[160];
		snprintf(q, sizeof(q), "/bin/%s", e->d_name);
		snprintf(r, sizeof(r), "%s/%s", p, e->d_name);
		if (exists(q)) {
			closedir(d);
			return;
		}
	}
	if (d) {
		closedir(d);
	}
	if (bind(p, "/bin", BIND_FLAG_AFTER) < 0) {
		fprintf(stderr, "pkg: cannot add %s to /bin: %s\n", p, strerror(errno));
	}
}

static void remove_files(const char *name, int quiet);

static int install(const char *name)
{
	struct pkg *k = find(name);
	if (!k) {
		fprintf(stderr, "pkg: no package %s (pkg search, or pkg update)\n", name);
		return 1;
	}
	char have[32];
	installed_version(name, have, sizeof(have));
	if (have[0] && !strcmp(have, k->version)) {
		printf("pkg: %s %s is already installed\n", name, have);
		return 0;
	}
	if (strcmp(k->kind, "dos") && strcmp(k->kind, "native")) {
		fprintf(stderr, "pkg: %s: unknown kind %s (a newer pkg may be needed)\n", name, k->kind);
		return 1;
	}
	char tar[160], part[160], url[256];
	snprintf(part, sizeof(part), "%s/cache/%s.prt", root, name);
	snprintf(tar, sizeof(tar), "%s/cache/%s.tar", root, name);
	snprintf(url, sizeof(url), "%s/%s", repo, k->file);
	printf("pkg: fetching %s %s (%ld bytes)\n", name, k->version, k->size);
	struct download d;
	d.fd = open(part, OWRITE | O_CREAT | O_TRUNC);
	if (d.fd < 0) {
		fprintf(stderr, "pkg: %s: %s\n", part, strerror(errno));
		return 1;
	}
	sha256_init(&d.h);
	d.got = 0;
	struct http_result res;
	int rc = http_get(url, to_file, &d, &res);
	close(d.fd);
	if (rc || res.status != 200) {
		fprintf(stderr, "pkg: %s: %s", url, rc ? res.error : "HTTP ");
		if (!rc) {
			fprintf(stderr, "%d", res.status);
		}
		fprintf(stderr, "\n");
		unlink(part);
		return 1;
	}
	uint8_t digest[32];
	char hex[65];
	sha256_final(&d.h, digest);
	for (int i = 0; i < 32; i++) {
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	}
	if (d.got != k->size || strcmp(hex, k->sha256)) {
		fprintf(stderr, "pkg: %s: the download does not match the signed index "
		                "(%ld bytes, sha256 %.16s...); not installing\n", name, d.got, hex);
		unlink(part);
		return 1;
	}
	unlink(tar);
	if (rename(part, tar) < 0) {
		fprintf(stderr, "pkg: %s: %s\n", tar, strerror(errno));
		return 1;
	}
	if (have[0]) {
		remove_files(name, 1); /* an upgrade: the old files go first */
	}
	char lst[160], tmp[160];
	snprintf(lst, sizeof(lst), "%s/db/%s.lst", root, name);
	snprintf(tmp, sizeof(tmp), "%s/db/%s.new", root, name);
	FILE *list = fopen(tmp, "w");
	if (!list) {
		fprintf(stderr, "pkg: %s: %s\n", tmp, strerror(errno));
		return 1;
	}
	fprintf(list, "%s %s\n", name, k->version);
	rc = extract(k, tar, list);
	for (int i = 0; rc == 0 && i < k->ncmds; i++) {
		char launcher[160], text[400];
		snprintf(launcher, sizeof(launcher), "%s/bin/%s", root, k->cmd[i]);
		if (!strcmp(k->kind, "dos")) {
			int n = snprintf(text, sizeof(text),
			                 "#!/bin/dosrun --launcher\n# pkg: %s %s\nprogram %s/dos/%s/%s\n",
			                 name, k->version, root, name, k->target[i]);
			for (int e = 0; e < k->nenv && n > 0 && n < (int)sizeof(text); e++) {
				const char *line = k->env[e];
				if (!strncmp(line, "envpath ", 8)) {
					const char *eq = strchr(line, '=');
					n += snprintf(text + n, sizeof(text) - (size_t)n, "envpath %.*s=%s/dos/%s/%s\n",
					              (int)(eq - line - 8), line + 8, root, name, eq + 1);
				} else {
					n += snprintf(text + n, sizeof(text) - (size_t)n, "%s\n", line);
				}
			}
			if (n <= 0 || n >= (int)sizeof(text)) {
				rc = -1;
				break;
			}
			if (write_file(launcher, text, (size_t)n) < 0) {
				rc = -1;
			}
			fprintf(list, "bin/%s\n", k->cmd[i]);
		}
	}
	fclose(list);
	unlink(tar);
	if (rc) {
		fprintf(stderr, "pkg: %s: installing failed; removing what was installed\n", name);
		unlink(lst);
		rename(tmp, lst);
		remove_files(name, 1);
		return 1;
	}
	unlink(lst);
	if (rename(tmp, lst) < 0) {
		fprintf(stderr, "pkg: %s: %s\n", lst, strerror(errno));
		return 1;
	}
	bind_bin();
	printf("pkg: installed %s %s", name, k->version);
	if (k->ncmds) {
		printf(" -- run:");
		for (int i = 0; i < k->ncmds; i++) {
			printf(" %s", k->cmd[i]);
		}
	}
	printf("\n");
	return 0;
}

/* Removes the files DB/NAME.LST lists, its empty directories, and the
 * list itself. */
static void remove_files(const char *name, int quiet)
{
	char lst[160];
	snprintf(lst, sizeof(lst), "%s/db/%s.lst", root, name);
	FILE *f = fopen(lst, "r");
	if (!f) {
		if (!quiet) {
			fprintf(stderr, "pkg: %s is not installed\n", name);
		}
		return;
	}
	char line[256];
	int first = 1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (first) {
			first = 0;
			continue;
		}
		if (!*line || strstr(line, "..")) {
			continue;
		}
		char p[384];
		snprintf(p, sizeof(p), "%s/%s", root, line);
		unlink(p);
		/* Prune empty parents up to (not including) bin, dos and share. */
		for (char *s = strrchr(p, '/'); s && s > p + strlen(root) + 4; s = strrchr(p, '/')) {
			*s = 0;
			if (rmdir(p) < 0) {
				break;
			}
		}
	}
	fclose(f);
	unlink(lst);
}

/* --- commands --- */

static void list_installed(void)
{
	char p[160];
	path_of(p, sizeof(p), root, "db");
	DIR *d = opendir(p);
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d))) {
		char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcmp(dot, ".lst") && strcmp(dot, ".LST"))) {
			continue;
		}
		char name[16], ver[32];
		snprintf(name, sizeof(name), "%.*s", (int)(dot - e->d_name), e->d_name);
		for (char *c = name; *c; c++) {
			if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
		}
		installed_version(name, ver, sizeof(ver));
		printf("%-8s %s\n", name, ver);
		n++;
	}
	if (d) {
		closedir(d);
	}
	if (!n) {
		printf("pkg: nothing installed in %s\n", root);
	}
}

static void show(const struct pkg *k, int long_form)
{
	char have[32];
	installed_version(k->name, have, sizeof(have));
	printf("%-8s %-12s %-6s %s%s\n", k->name, k->version, k->kind, k->title,
	       have[0] ? " [installed]" : "");
	if (long_form) {
		printf("  %s\n  License: %s\n  Size: %ld bytes\n", k->summary, k->license, k->size);
		if (k->status[0]) {
			printf("  On ManiOS: %s\n", k->status);
		}
		if (!strcmp(k->kind, "dos")) {
			printf("  Runs through dosrun (a DOS program)\n");
		}
		for (int i = 0; i < k->ncmds; i++) {
			printf("  Command: %s\n", k->cmd[i]);
		}
	}
}

static int ci_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i]) {
			char a = hay[i], b = needle[i];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) {
				break;
			}
			i++;
		}
		if (i == n) {
			return 1;
		}
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: pkg [-r ROOT] [-u URL] update | list [-i] | search WORD | "
	                "info NAME | install NAME... | remove NAME... | upgrade\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int i = 1;
	for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
		if (!strcmp(argv[i], "-r")) {
			snprintf(root, sizeof(root), "%s", argv[i + 1]);
		} else if (!strcmp(argv[i], "-u")) {
			repo = argv[i + 1];
			repo_given = 1;
		} else {
			usage();
		}
	}
	if (i >= argc) {
		usage();
	}
	const char *cmd = argv[i++];
	find_root();
	root_dirs();
	if (!repo_given) {
		char p[160];
		path_of(p, sizeof(p), root, "repo.url");
		long n = read_file(p, saved_repo, sizeof(saved_repo) - 1);
		if (n > 7) {
			saved_repo[n] = 0;
			saved_repo[strcspn(saved_repo, "\r\n")] = 0;
			repo = saved_repo;
		}
	}
	if (!strcmp(cmd, "update")) {
		update();
		return 0;
	}
	if (!strcmp(cmd, "list") && i < argc && !strcmp(argv[i], "-i")) {
		list_installed();
		return 0;
	}
	if (!strcmp(cmd, "remove")) {
		if (i >= argc) {
			usage();
		}
		for (; i < argc; i++) {
			char have[32];
			installed_version(argv[i], have, sizeof(have));
			if (!have[0]) {
				fprintf(stderr, "pkg: %s is not installed\n", argv[i]);
				continue;
			}
			remove_files(argv[i], 0);
			printf("pkg: removed %s %s\n", argv[i], have);
		}
		return 0;
	}
	load_index();
	if (!strcmp(cmd, "list")) {
		for (int k = 0; k < npkgs; k++) {
			show(&pkgs[k], 0);
		}
		return 0;
	}
	if (!strcmp(cmd, "search")) {
		if (i >= argc) {
			usage();
		}
		int n = 0;
		for (int k = 0; k < npkgs; k++) {
			if (ci_contains(pkgs[k].name, argv[i]) || ci_contains(pkgs[k].title, argv[i])
			    || ci_contains(pkgs[k].summary, argv[i])) {
				show(&pkgs[k], 0);
				n++;
			}
		}
		if (!n) {
			printf("pkg: nothing matches %s\n", argv[i]);
		}
		return 0;
	}
	if (!strcmp(cmd, "info")) {
		if (i >= argc) {
			usage();
		}
		struct pkg *k = find(argv[i]);
		if (!k) {
			fprintf(stderr, "pkg: no package %s\n", argv[i]);
			return 1;
		}
		show(k, 1);
		return 0;
	}
	if (!strcmp(cmd, "install")) {
		if (i >= argc) {
			usage();
		}
		int failed = 0;
		for (; i < argc; i++) {
			failed |= install(argv[i]);
		}
		return failed;
	}
	if (!strcmp(cmd, "upgrade")) {
		int failed = 0, n = 0;
		for (int k = 0; k < npkgs; k++) {
			char have[32];
			installed_version(pkgs[k].name, have, sizeof(have));
			if (have[0] && strcmp(have, pkgs[k].version)) {
				failed |= install(pkgs[k].name);
				n++;
			}
		}
		if (!n) {
			printf("pkg: everything installed is up to date\n");
		}
		return failed;
	}
	usage();
	return 2;
}
