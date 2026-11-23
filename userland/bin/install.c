/* install [-y] [OPTIONS] DISK [KEY=VALUE...] -- install ManiOS on a hard
 * disk (M14, ADR-0006; profiles and a data partition: 0.30.0, ADR-0011).
 *
 * Without a DISK, install asks, step by step, as Debian's installer does:
 * keyboard, network, host name, disk, profile, where packages come from,
 * then a summary to confirm. The same choices as options:
 *
 *   --profile desktop|server|server-dos|custom
 *   --pkg NAME,NAME...      more packages (any profile)
 *   --hostname NAME         sysname=
 *   --net dhcp|A.B.C.D/N[,GATEWAY]   --dns A.B.C.D
 *   --repo cd|URL           where setup gets the packages (default: the
 *                           CD's REPO when there is one, else manios.nl)
 *
 * With a profile, install writes the base system (BASE.BIN from the
 * install CD, or this system's boot area when there is none) and a FAT16
 * data partition after it, up to 2 GiB, holding SETUP.CFG; the installed
 * system's rc= is /boot/etc/rc.setup, which runs setup at its first start
 * to install the profile's packages (userland/bin/setup.c). Without a
 * profile, install does what it always did:
 *
 * Copies the boot area this system was started from (/dev/bootarea:
 * the ManiOS boot loader and kernel) to DISK (ata0, sata0, vd0, ...) and makes
 * the disk boot it: an MBR with the ManiOS boot code, and one partition
 * of type 0xDA holding the boot area, at 1 MiB. KEY=VALUE words become
 * the installed system's kernel command line (sysname=, ip=, key=, rc=,
 * export=, ...); the boot loader lets you change it at each boot.
 *
 * Everything on DISK is lost: install asks you to type "yes" first,
 * unless -y. It reads everything back to check it. */
#include <bootarea.h>
#include <ctype.h>
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define MKFAT_LIBRARY
#include "mkfat.c"

#define SECTOR 512
#define START_SECTOR 2048          /* the boot partition begins at 1 MiB */
#define ALIGN_SECTORS 2048         /* and ends on a MiB boundary */
#define CHUNK (64 * 1024)

static int fail(const char *what, const char *detail)
{
	fprintf(stderr, "install: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

static int read_all(int fd, uint8_t *buf, uint32_t len)
{
	uint32_t done = 0;
	while (done < len) {
		long n = read(fd, buf + done, len - done > CHUNK ? CHUNK : len - done);
		if (n <= 0) {
			return -1;
		}
		done += (uint32_t)n;
	}
	return 0;
}

static int write_at(int fd, uint32_t offset, const uint8_t *buf, uint32_t len)
{
	if (lseek(fd, (long)offset, SEEK_SET) != (long)offset) {
		return -1;
	}
	uint32_t done = 0;
	while (done < len) {
		long n = write(fd, buf + done, len - done > CHUNK ? CHUNK : len - done);
		if (n <= 0) {
			return -1;
		}
		done += (uint32_t)n;
	}
	return 0;
}

/* Reads back len bytes at offset and compares them with buf. */
static int same_at(int fd, uint32_t offset, const uint8_t *buf, uint32_t len)
{
	static uint8_t back[CHUNK];
	if (lseek(fd, (long)offset, SEEK_SET) != (long)offset) {
		return 0;
	}
	for (uint32_t done = 0; done < len;) {
		uint32_t n = len - done > CHUNK ? CHUNK : len - done;
		if (read_all(fd, back, n) < 0 || memcmp(back, buf + done, n)) {
			return 0;
		}
		done += n;
	}
	return 1;
}

static int confirm(void)
{
	char line[16];
	printf("Type yes to go on: ");
	fflush(stdout);
	if (!fgets(line, sizeof(line), stdin)) {
		return 0;
	}
	line[strcspn(line, "\n")] = '\0';
	return !strcmp(line, "yes");
}

#define DATA_MIN_SECTORS (8 * 2048)   /* 8 MiB */
#define DATA_MAX_SECTORS 4190000       /* FAT16's 2 GiB, a little under */

struct profile {
	const char *name, *title, *packages, *shell;
};

static const struct profile PROFILES[] = {
	{ "desktop", "Desktop: ManiDE and ManiDOS", "manide manidos", "manide" },
	{ "server", "Server: the shell, the network and pkg", "", "" },
	{ "server-dos", "Server + DOS: ManiDOS and the DOS tools", "manidos @dos-tools", "" },
	{ "custom", "Custom: the packages you name", "", "" },
};
#define NPROFILES (int)(sizeof(PROFILES) / sizeof(PROFILES[0]))

struct choices {
	const char *disk;
	const struct profile *profile;
	char packages[300];
	char hostname[40];
	char net[48], dns[20];
	char repo[160];
	char extra[BA_CMDLINE_MAX]; /* KEY=VALUE words given as they are */
};

/* Checks a boot area read into memory; NULL if good, else why not. */
static const char *bad_area(const uint8_t *area, uint32_t total)
{
	if (total < BOOTAREA_ALIGN || memcmp(area, BOOTAREA_MAGIC, 8)
	    || bootarea_u32(area, BA_VERSION) != BOOTAREA_VERSION
	    || bootarea_u32(area, BA_TOTAL) != total
	    || bootarea_crc32(0, area + BOOTAREA_ALIGN, total - BOOTAREA_ALIGN)
	           != bootarea_u32(area, BA_CRC32)) {
		return "damaged";
	}
	uint32_t mbr_off = bootarea_u32(area, BA_MBR_OFF);
	if (bootarea_u32(area, BA_MBR_LEN) != 446 || mbr_off > total - 446) {
		return "without MBR boot code";
	}
	return NULL;
}

static uint8_t *read_area(const char *path, uint32_t *total)
{
	int fd = open(path, OREAD);
	struct zkt_dirent st;
	if (fd < 0 || fstat(fd, &st) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	uint8_t *area = malloc(st.size ? st.size : 1);
	if (!area || read_all(fd, area, st.size) < 0) {
		close(fd);
		free(area);
		return NULL;
	}
	close(fd);
	*total = st.size;
	return area;
}

/* The first /n/X that has NAME (BASE.BIN, REPO/INDEX.TXT): its /n/X. */
static int find_on_disks(const char *name, char *out, size_t n)
{
	DIR *d = opendir("/n");
	struct dirent *e;
	int found = 0;
	while (!found && d && (e = readdir(d))) {
		char p[96];
		snprintf(p, sizeof(p), "/n/%s/%s", e->d_name, name);
		int fd = open(p, OREAD);
		if (fd >= 0) {
			close(fd);
			snprintf(out, n, "/n/%s", e->d_name);
			found = 1;
		}
	}
	if (d) {
		closedir(d);
	}
	return found;
}

static int is_disk_name(const char *disk)
{
	if (strncmp(disk, "ata", 3) && strncmp(disk, "sata", 4) && strncmp(disk, "vd", 2)) {
		return 0;
	}
	const char *p = strrchr(disk, 'p');
	return !(p && p > disk && isdigit((unsigned char)p[-1]) && isdigit((unsigned char)p[1]));
}

static void append_word(char *line, size_t n, const char *word)
{
	if (word[0] && strlen(line) + strlen(word) + 2 <= n) {
		if (line[0]) {
			strcat(line, " ");
		}
		strcat(line, word);
	}
}

#define MAX_FILES 100

/* Reads CD/REPO's INDEX.TXT, INDEX.SIG and each PKGS/NAME.TAR into files (root
 * directory names: INDEX.TXT, INDEX.SIG, NAME.TAR). Returns how many. */
static int copy_repo(const char *cd, struct file_in *files, int max)
{
	char dir[96], p[160];
	int n = 0;
	const char *top[] = { "INDEX.TXT", "INDEX.SIG" };
	for (int i = 0; i < 2; i++) {
		snprintf(p, sizeof(p), "%s/REPO/%s", cd, top[i]);
		uint32_t size;
		uint8_t *data = read_area(p, &size);
		if (!data) {
			return 0;
		}
		name83(top[i], files[n].name83);
		files[n].data = data;
		files[n].size = size;
		n++;
	}
	snprintf(dir, sizeof(dir), "%s/REPO/PKGS", cd);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		if (e->d_name[0] == '.' || name83(e->d_name, files[n].name83) < 0) {
			continue;
		}
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		uint32_t size;
		uint8_t *data = read_area(p, &size);
		if (!data) {
			continue;
		}
		files[n].data = data;
		files[n].size = size;
		n++;
	}
	if (d) {
		closedir(d);
	}
	return n;
}

/* --- asking, step by step --- */

static void ask(const char *question, const char *def, char *out, size_t n)
{
	printf("%s [%s]: ", question, def);
	fflush(stdout);
	char line[200];
	if (!fgets(line, sizeof(line), stdin)) {
		line[0] = 0;
	}
	line[strcspn(line, "\r\n")] = 0;
	snprintf(out, n, "%s", line[0] ? line : def);
}

static void step(int n, const char *title)
{
	printf("\n-- Step %d of 7: %s --\n", n, title);
}

static int interactive(struct choices *c)
{
	static char disk[24];
	char answer[200], first[24] = "";
	char version[17] = "";
	uint32_t total;
	uint8_t *self = read_area("/dev/bootarea", &total);
	if (self && total > BA_OS_VERSION + 16) {
		memcpy(version, self + BA_OS_VERSION, 16);
	}
	free(self);
	printf("\nManiOS %s installer\n"
	       "Each step has a default in [brackets]: Enter takes it.\n", version);

	step(1, "keyboard");
	printf("US layout (the only one ManiOS has so far).\n");

	step(2, "network");
	ask("Address: dhcp, or A.B.C.D/N", "dhcp", c->net, sizeof(c->net));
	if (strcmp(c->net, "dhcp")) {
		ask("Gateway (A.B.C.D, or none)", "none", answer, sizeof(answer));
		if (strcmp(answer, "none")) {
			size_t l = strlen(c->net);
			snprintf(c->net + l, sizeof(c->net) - l, ",%s", answer);
		}
	}
	ask("DNS server (A.B.C.D, or auto from DHCP)", "auto", c->dns, sizeof(c->dns));
	if (!strcmp(c->dns, "auto")) {
		c->dns[0] = 0;
	}

	step(3, "host name");
	ask("This machine's name", "manios", c->hostname, sizeof(c->hostname));

	step(4, "disk");
	printf("Hard disks:\n");
	DIR *d = opendir("/dev");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!is_disk_name(e->d_name)) {
			continue;
		}
		char p[40];
		struct zkt_dirent st;
		snprintf(p, sizeof(p), "/dev/%s", e->d_name);
		int fd = open(p, OREAD);
		if (fd >= 0 && fstat(fd, &st) == 0 && st.type == ZKT_TYPE_DEVICE) {
			printf("  %-8s %lu MiB\n", e->d_name, (unsigned long)(st.size / (1024 * 1024)));
			if (!first[0]) {
				snprintf(first, sizeof(first), "%s", e->d_name);
			}
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	if (d) {
		closedir(d);
	}
	if (!first[0]) {
		fprintf(stderr, "install: there is no hard disk to install on\n");
		return 1;
	}
	ask("Install on", first, disk, sizeof(disk));
	c->disk = disk;
	printf("The whole disk is used: the system, then a data partition for\n"
	       "packages and your files (FAT, up to 2 GiB).\n");

	step(5, "profile");
	for (int i = 0; i < NPROFILES; i++) {
		printf("  %d  %s\n", i + 1, PROFILES[i].title);
	}
	ask("Profile", "1", answer, sizeof(answer));
	int k = atoi(answer);
	for (int i = 0; i < NPROFILES; i++) {
		if (!strcmp(answer, PROFILES[i].name)) {
			k = i + 1;
		}
	}
	if (k < 1 || k > NPROFILES) {
		fprintf(stderr, "install: no profile %s\n", answer);
		return 1;
	}
	c->profile = &PROFILES[k - 1];
	ask("More packages (names, or none)", "none", answer, sizeof(answer));
	if (strcmp(answer, "none")) {
		snprintf(c->packages, sizeof(c->packages), "%s", answer);
	}

	step(6, "packages");
	char cd[64];
	int have_cd = find_on_disks("REPO/INDEX.TXT", cd, sizeof(cd));
	if (have_cd) {
		printf("The install CD has the package repository (%s/REPO).\n", cd);
	}
	ask("Get packages from: cd, or a repository URL", have_cd ? "cd" : "http://manios.nl/repo",
	    c->repo, sizeof(c->repo));
	return 0;
}

/* --- installing --- */

int main(int argc, char **argv)
{
	int yes = 0, a = 1;
	struct choices c;
	memset(&c, 0, sizeof(c));
	for (; a < argc && argv[a][0] == '-'; a++) {
		const char *o = argv[a], *v = a + 1 < argc ? argv[a + 1] : NULL;
		if (!strcmp(o, "-y")) {
			yes = 1;
			continue;
		}
		if (!strcmp(o, "-h") || !strcmp(o, "--help") || !v) {
			a = argc;
			break;
		}
		if (!strcmp(o, "--profile")) {
			for (int i = 0; i < NPROFILES; i++) {
				if (!strcmp(v, PROFILES[i].name)) {
					c.profile = &PROFILES[i];
				}
			}
			if (!c.profile) {
				return fail("no such profile (desktop, server, server-dos, custom)", v);
			}
		} else if (!strcmp(o, "--pkg")) {
			snprintf(c.packages, sizeof(c.packages), "%s", v);
			for (char *p = c.packages; *p; p++) {
				*p = *p == ',' ? ' ' : *p;
			}
		} else if (!strcmp(o, "--hostname")) {
			snprintf(c.hostname, sizeof(c.hostname), "%s", v);
		} else if (!strcmp(o, "--net")) {
			snprintf(c.net, sizeof(c.net), "%s", v);
		} else if (!strcmp(o, "--dns")) {
			snprintf(c.dns, sizeof(c.dns), "%s", v);
		} else if (!strcmp(o, "--repo")) {
			snprintf(c.repo, sizeof(c.repo), "%s", v);
		} else {
			return fail("unknown option", o);
		}
		a++;
	}
	if (argc == 1) {
		if (interactive(&c)) {
			return 1;
		}
	} else if (a >= argc) {
		fprintf(stderr,
		        "usage: install [-y] DISK [KEY=VALUE...]\n"
		        "       install [-y] --profile desktop|server|server-dos|custom [--pkg A,B]\n"
		        "               [--hostname NAME] [--net dhcp|A.B.C.D/N[,GW]] [--dns A.B.C.D]\n"
		        "               [--repo cd|URL] DISK [KEY=VALUE...]\n"
		        "       install            (asks step by step)\n"
		        "  DISK: a whole hard disk, such as ata0, sata0 or vd0 (see ls /dev)\n");
		return 2;
	} else {
		c.disk = argv[a++];
	}
	if ((c.packages[0] || c.hostname[0] || c.net[0] || c.dns[0] || c.repo[0]) && !c.profile) {
		c.profile = &PROFILES[3]; /* options without a profile: custom */
	}
	const char *disk = c.disk;
	if (!strncmp(disk, "/dev/", 5)) {
		disk += 5;
	}
	char *cmdline = c.extra;
	for (; a < argc; a++) {
		if (!strchr(argv[a], '=')) {
			return fail("not KEY=VALUE", argv[a]);
		}
		if (strlen(cmdline) + strlen(argv[a]) + 2 > sizeof(c.extra)) {
			return fail("the command line is too long", 0);
		}
		append_word(cmdline, sizeof(c.extra), argv[a]);
	}

	/* What to install: with a profile, the base system from the install
	 * CD; otherwise (or without a CD) the boot area this system came from. */
	uint32_t total = 0;
	uint8_t *area = NULL;
	char from[96] = "/dev/bootarea", where[64];
	if (c.profile && find_on_disks("BASE.BIN", where, sizeof(where))) {
		snprintf(from, sizeof(from), "%s/BASE.BIN", where);
	}
	area = read_area(from, &total);
	if (!area && strcmp(from, "/dev/bootarea")) {
		snprintf(from, sizeof(from), "/dev/bootarea");
		area = read_area(from, &total);
	}
	if (!area) {
		return fail("there is no /dev/bootarea: this system was not started by the ManiOS "
		            "boot loader (from a ManiOS CD or disk), so there is nothing to install",
		            0);
	}
	const char *why = bad_area(area, total);
	if (why) {
		char what[120];
		snprintf(what, sizeof(what), "%s is %s", from, why);
		return fail(what, 0);
	}
	uint32_t mbr_off = bootarea_u32(area, BA_MBR_OFF);
	char version[17] = "";
	memcpy(version, area + BA_OS_VERSION, 16);

	/* Where: a whole hard disk (IDE, SATA or virtio), big enough. A
	 * partition's name is its disk's and "pN" (ata0p1, sata0p2). */
	const char *p = strrchr(disk, 'p');
	if (p && p > disk && isdigit((unsigned char)p[-1]) && isdigit((unsigned char)p[1])) {
		return fail("install on a whole disk (such as ata0), not a partition", disk);
	}
	char path[32];
	struct zkt_dirent st;
	snprintf(path, sizeof(path), "/dev/%s", disk);
	int dst = open(path, ORDWR);
	if (dst < 0 || fstat(dst, &st) < 0) {
		return fail(path, strerror(errno));
	}
	if (st.type != ZKT_TYPE_DEVICE) {
		return fail("not a disk", path);
	}
	if (!is_disk_name(disk)) {
		return fail("install on a hard disk (such as ata0, sata0 or vd0)", disk);
	}
	uint32_t disk_sectors = st.size / SECTOR;
	uint32_t area_sectors = (total + SECTOR - 1) / SECTOR;
	uint32_t part_sectors = (area_sectors + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
	uint32_t data_start = START_SECTOR + part_sectors, data_sectors = 0;
	uint32_t need = START_SECTOR + part_sectors + (c.profile ? DATA_MIN_SECTORS : 0);
	if (disk_sectors < need) {
		char needs[48];
		snprintf(needs, sizeof(needs), "it needs %lu KiB", (unsigned long)need / 2);
		return fail("the disk is too small", needs);
	}
	if (c.profile) {
		data_sectors = disk_sectors - data_start;
		data_sectors = data_sectors > DATA_MAX_SECTORS ? DATA_MAX_SECTORS : data_sectors;
	}

	/* The installed system's command line. */
	char line[BA_CMDLINE_MAX] = "";
	char packages[400] = "";
	if (c.profile) {
		char w[80];
		if (c.hostname[0]) {
			snprintf(w, sizeof(w), "sysname=%s", c.hostname);
			append_word(line, sizeof(line), w);
		}
		if (c.net[0] && strcmp(c.net, "dhcp")) {
			char ip[48], *gw;
			snprintf(ip, sizeof(ip), "%s", c.net);
			gw = strchr(ip, ',');
			if (gw) {
				*gw++ = 0;
			}
			snprintf(w, sizeof(w), "ip=%s", ip);
			append_word(line, sizeof(line), w);
			if (gw) {
				snprintf(w, sizeof(w), "gw=%s", gw);
				append_word(line, sizeof(line), w);
			}
		}
		if (c.dns[0]) {
			snprintf(w, sizeof(w), "dns=%s", c.dns);
			append_word(line, sizeof(line), w);
		}
		if (c.profile->shell[0]) {
			snprintf(w, sizeof(w), "shell=%s", c.profile->shell);
			append_word(line, sizeof(line), w);
		}
		append_word(line, sizeof(line), "rc=/boot/etc/rc.setup");
		snprintf(packages, sizeof(packages), "%s", c.profile->packages);
		if (c.packages[0]) {
			append_word(packages, sizeof(packages), c.packages);
		}
		if (!c.repo[0]) {
			char cd[64];
			snprintf(c.repo, sizeof(c.repo), "%s",
			         find_on_disks("REPO/INDEX.TXT", cd, sizeof(cd)) ? "cd" : "http://manios.nl/repo");
		}
	}
	if (strlen(line) + strlen(cmdline) + 2 > sizeof(line)) {
		return fail("the command line is too long", 0);
	}
	append_word(line, sizeof(line), cmdline);

	printf("\nManiOS %s will be installed on %s (%lu MiB).\n"
	       "EVERYTHING ON %s WILL BE LOST.\n",
	       version, disk, (unsigned long)(disk_sectors / 2048), disk);
	if (c.profile) {
		printf("  profile:   %s\n"
		       "  system:    %s (%lu KiB)\n"
		       "  data:      FAT16, %lu MiB (%sp2)\n"
		       "  packages:  %s\n"
		       "  from:      %s\n",
		       c.profile->title, from, (unsigned long)(total / 1024),
		       (unsigned long)(data_sectors / 2048), disk, packages[0] ? packages : "none",
		       packages[0] ? c.repo : "-");
	}
	printf("The installed system's command line: \"%s\"\n", line);
	if (!yes && !confirm()) {
		printf("Nothing was written.\n");
		return 1;
	}

	/* The data partition first (formatted, with SETUP.CFG), then the boot
	 * area, the MBR last: a disk left half-written doesn't look bootable. */
	if (c.profile) {
		/* With packages from the CD: its signed index and every package
		 * go onto the data partition too, so the CD can come out now;
		 * setup installs from them and deletes them. */
		static struct file_in files[MAX_FILES];
		int nfiles = 1;
		const char *repo = c.repo;
		char cd[64];
		if (packages[0] && !strcmp(c.repo, "cd")) {
			if (!find_on_disks("REPO/INDEX.TXT", cd, sizeof(cd))) {
				return fail("--repo cd, but there is no install CD with a REPO", 0);
			}
			nfiles = copy_repo(cd, files + 1, MAX_FILES - 1) + 1;
			if (nfiles == 1) {
				return fail("cannot read the CD's REPO", cd);
			}
			repo = "disk";
		}
		char cfg[700];
		int n = snprintf(cfg, sizeof(cfg),
		                 "# ManiOS setup (install, 0.30.0): setup reads this at the first start.\r\n"
		                 "profile=%s\r\npackages=%s\r\nrepo=%s\r\nfallback=http://manios.nl/repo\r\n",
		                 c.profile->name, packages, repo);
		name83("SETUP.CFG", files[0].name83);
		files[0].data = (uint8_t *)cfg;
		files[0].size = (uint32_t)n;
		printf("formatting the data partition%s...\n",
		       nfiles > 1 ? " and copying the CD's packages" : "");
		if (mkfat_format(dst, path, data_start, data_sectors, "MANIOS", files, nfiles, 0)) {
			return fail("formatting the data partition failed", 0);
		}
	}
	memset(area + BA_CMDLINE, 0, BA_CMDLINE_MAX);
	memcpy(area + BA_CMDLINE, line, strlen(line));
	uint8_t mbr[SECTOR];
	memset(mbr, 0, sizeof(mbr));
	memcpy(mbr, area + mbr_off, 446);
	for (int part = 0; part < (c.profile ? 2 : 1); part++) {
		uint8_t *entry = mbr + 446 + 16 * part;
		uint32_t first = part ? data_start : START_SECTOR;
		uint32_t count = part ? data_sectors : part_sectors;
		entry[0] = part ? 0x00 : 0x80; /* the boot partition is the active one */
		entry[1] = 0xFE, entry[2] = 0xFF, entry[3] = 0xFF; /* CHS: "use the LBA" */
		entry[4] = part ? 0x06 : BOOTAREA_PARTITION_TYPE; /* FAT16 */
		entry[5] = 0xFE, entry[6] = 0xFF, entry[7] = 0xFF;
		for (int i = 0; i < 4; i++) {
			entry[8 + i] = (uint8_t)(first >> (8 * i));
			entry[12 + i] = (uint8_t)(count >> (8 * i));
		}
	}
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	printf("writing the boot area (%lu KiB)...\n", (unsigned long)(total / 1024));
	if (write_at(dst, START_SECTOR * SECTOR, area, total) < 0
	    || write_at(dst, 0, mbr, sizeof(mbr)) < 0) {
		return fail("writing the disk failed", strerror(errno));
	}
	printf("checking what was written...\n");
	if (!same_at(dst, START_SECTOR * SECTOR, area, total) || !same_at(dst, 0, mbr, sizeof(mbr))) {
		return fail("the disk doesn't read back what was written", 0);
	}
	close(dst);
	printf("ManiOS %s is installed on %s. Remove the CD and restart the machine.\n", version,
	       disk);
	if (c.profile && packages[0]) {
		printf("At its first start, setup installs the packages, then restarts once.\n");
	}
	return 0;
}
