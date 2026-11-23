/* setup -- finishes an installation at its first start (0.30.0,
 * ADR-0011), as Debian's base-config once did.
 *
 * install writes the base system and a FAT data partition holding
 * SETUP.CFG: the profile's packages and where to get them. Installed
 * systems start /boot/etc/rc.setup, which runs setup at every boot:
 *
 *   - with a SETUP.CFG on a disk: install its packages (pkg) into that
 *     disk's manios/pkg -- from the copy of the CD's packages install
 *     left beside it (repo=disk; deleted afterwards), the install CD's
 *     REPO (repo=cd) or a URL, falling back to the network; make HOME; keep the file as SETUP.DON;
 *     then restart once, so the new commands (and shell=manide) are there
 *     from the start. When it fails, SETUP.CFG stays for the next boot.
 *   - otherwise: run the disk's manios/etc/rc, if there is one.
 *
 * SETUP.CFG is KEY=VALUE lines: profile=NAME, packages=NAME NAME...,
 * repo=disk|cd|URL, fallback=URL. */
#include <dirent.h>
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FALLBACK "http://manios.nl/repo"
#define BATCH 12 /* a command takes at most 16 arguments */

static char profile[32], packages[512], repo[200], fallback[200] = FALLBACK;

static int is_file(const char *p)
{
	int fd = open(p, OREAD);
	if (fd < 0) {
		return 0;
	}
	close(fd);
	return 1;
}

static int run(char **argv)
{
	int pid = spawn(argv[0], argv), status = 1;
	if (pid < 0) {
		fprintf(stderr, "setup: %s: %s\n", argv[0], strerror(errno));
		return 1;
	}
	if (wait(pid, &status) != pid) {
		return 1;
	}
	return status;
}

static void read_cfg(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[600];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq++ = 0;
		if (!strcmp(line, "profile")) {
			snprintf(profile, sizeof(profile), "%s", eq);
		} else if (!strcmp(line, "packages")) {
			snprintf(packages, sizeof(packages), "%s", eq);
		} else if (!strcmp(line, "repo")) {
			snprintf(repo, sizeof(repo), "%s", eq);
		} else if (!strcmp(line, "fallback")) {
			snprintf(fallback, sizeof(fallback), "%s", eq);
		}
	}
	if (f) {
		fclose(f);
	}
}

/* The install CD's repository: the first /n/X/REPO with an index. */
static int find_cd(char *out, size_t n)
{
	DIR *d = opendir("/n");
	struct dirent *e;
	int found = 0;
	while (!found && d && (e = readdir(d))) {
		char p[96];
		snprintf(p, sizeof(p), "/n/%s/REPO/INDEX.TXT", e->d_name);
		if (is_file(p)) {
			snprintf(out, n, "/n/%s/REPO", e->d_name);
			found = 1;
		}
	}
	if (d) {
		closedir(d);
	}
	return found;
}

static int install_from(const char *root, const char *from)
{
	char *up[] = { "/bin/pkg", "-r", (char *)root, "-u", (char *)from, "update", 0 };
	printf("setup: packages from %s\n", from);
	if (run(up)) {
		return 1;
	}
	char list[512];
	snprintf(list, sizeof(list), "%s", packages);
	char *argv[6 + BATCH + 1] = { "/bin/pkg", "-r", (char *)root, "install" };
	int n = 4, failed = 0;
	for (char *w = strtok(list, " "); w; w = strtok(0, " ")) {
		argv[n++] = w;
		if (n == 4 + BATCH) {
			argv[n] = 0;
			failed |= run(argv);
			n = 4;
		}
	}
	if (n > 4) {
		argv[n] = 0;
		failed |= run(argv);
	}
	return failed;
}

/* Deletes the packages install copied into the disk's root. */
static void remove_copies(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char names[100][13];
	int n = 0;
	while (d && (e = readdir(d)) && n < 100) {
		size_t l = strlen(e->d_name);
		if ((l > 4 && (!strcmp(e->d_name + l - 4, ".TAR") || !strcmp(e->d_name + l - 4, ".tar")))
		    || !strcmp(e->d_name, "INDEX.TXT") || !strcmp(e->d_name, "INDEX.SIG")
		    || !strcmp(e->d_name, "index.txt") || !strcmp(e->d_name, "index.sig")) {
			snprintf(names[n++], 13, "%s", e->d_name);
		}
	}
	if (d) {
		closedir(d);
	}
	for (int i = 0; i < n; i++) {
		char p[128];
		snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
		unlink(p);
	}
}

static int finish(const char *disk)
{
	char cfg[96], done[96], root[96], p[128];
	snprintf(cfg, sizeof(cfg), "/n/%s/SETUP.CFG", disk);
	snprintf(done, sizeof(done), "/n/%s/SETUP.DON", disk);
	snprintf(root, sizeof(root), "/n/%s/manios/pkg", disk);
	read_cfg(cfg);
	printf("setup: finishing the installation on %s (profile %s)\n", disk,
	       profile[0] ? profile : "custom");
	int failed = 0;
	if (packages[0]) {
		char cd[96];
		const char *from = fallback;
		char here[96];
		snprintf(here, sizeof(here), "/n/%s", disk);
		if (!strcmp(repo, "disk")) {
			from = here; /* install copied the CD's packages here */
		} else if (!strcmp(repo, "cd")) {
			if (find_cd(cd, sizeof(cd))) {
				from = cd;
			} else {
				printf("setup: the install CD is not there; trying %s\n", fallback);
			}
		} else if (repo[0]) {
			from = repo;
		}
		failed = install_from(root, from);
		if (!failed && from == here) {
			remove_copies(here);
		}
		if (failed && from != fallback && strcmp(from, fallback)) {
			printf("setup: trying %s instead\n", fallback);
			failed = install_from(root, fallback);
		}
		/* Later updates come from the network, not the CD. */
		snprintf(p, sizeof(p), "%s/repo.url", root);
		int fd = open(p, OWRITE | O_CREAT | O_TRUNC);
		if (fd >= 0) {
			write(fd, fallback, strlen(fallback));
			close(fd);
		}
	}
	snprintf(p, sizeof(p), "/n/%s/home", disk);
	mkdir(p, 0755);
	if (failed) {
		printf("setup: some packages did not install; setup runs again at the next start\n"
		       "       (or run pkg install yourself)\n");
		return 1;
	}
	unlink(done);
	if (rename(cfg, done) < 0) {
		fprintf(stderr, "setup: %s: %s\n", cfg, strerror(errno));
		return 1;
	}
	if (packages[0]) {
		printf("setup: done; restarting once to start the installed system\n");
		char *rb[] = { "/bin/reboot", 0 };
		return run(rb);
	}
	printf("setup: done\n");
	return 0;
}

int main(void)
{
	DIR *d = opendir("/n");
	struct dirent *e;
	char disk[64] = "", rc[128] = "";
	while (d && (e = readdir(d))) {
		char p[96];
		snprintf(p, sizeof(p), "/n/%s/SETUP.CFG", e->d_name);
		if (!disk[0] && is_file(p)) {
			snprintf(disk, sizeof(disk), "%s", e->d_name);
		}
		snprintf(p, sizeof(p), "/n/%s/manios/etc/rc", e->d_name);
		if (!rc[0] && is_file(p)) {
			snprintf(rc, sizeof(rc), "%s", p);
		}
	}
	if (d) {
		closedir(d);
	}
	if (disk[0]) {
		return finish(disk);
	}
	if (rc[0]) {
		char *argv[] = { "/bin/sh", rc, 0 };
		return run(argv);
	}
	return 0;
}
