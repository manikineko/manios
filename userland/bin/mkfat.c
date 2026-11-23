/* mkfat [-y] [-l LABEL] [-f NAME=FILE]... DEVICE [START COUNT] -- make a
 * FAT16 file system (0.30.0, ADR-0011).
 *
 * On DEVICE (a partition such as ata0p2, or a whole disk), or on COUNT
 * sectors of it from sector START: the installer formats the new data
 * partition this way, before the kernel knows the partition exists.
 * -f puts small files in the root directory (the installer's SETUP.CFG).
 * Everything on that part of the device is lost: mkfat asks first,
 * unless -y.
 *
 * Layout: one reserved sector (the boot sector), two FATs, a 512-entry
 * root directory, then clusters. The cluster size is the smallest that
 * keeps the count within FAT16's 4085..65524, so the volume is 2 MiB to
 * 2 GiB.
 *
 * install includes this file (MKFAT_LIBRARY) for mkfat_format(). */
#include <errno.h>
#include <manios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define ROOT_ENTRIES 512
#define ROOT_SECTORS (ROOT_ENTRIES * 32 / SECTOR)
#define MIN_CLUSTERS 4085
#define MAX_CLUSTERS 65524
#define FILES_MAX 8

struct file_in {
	char name83[11];
	uint8_t *data;
	uint32_t size;
};

static int mkfat_fail(const char *what, const char *detail)
{
	fprintf(stderr, "mkfat: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

/* "NAME.EXT" as the 11 space-padded upper-case bytes of a directory
 * entry; -1 if it is not an 8.3 name. */
static int name83(const char *s, char out[11])
{
	memset(out, ' ', 11);
	const char *dot = strchr(s, '.');
	size_t base = dot ? (size_t)(dot - s) : strlen(s);
	size_t ext = dot ? strlen(dot + 1) : 0;
	if (!base || base > 8 || ext > 3 || (dot && strchr(dot + 1, '.'))) {
		return -1;
	}
	for (size_t i = 0; i < base + (dot ? 1 + ext : 0); i++) {
		char c = s[i];
		if (c == '.') {
			continue;
		}
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return -1;
		}
		if (i < base) {
			out[i] = c;
		} else {
			out[8 + i - base - 1] = c;
		}
	}
	return 0;
}

static int mkfat_write(int fd, uint32_t sector, const void *buf, uint32_t len)
{
	long off = (long)sector * SECTOR;
	if (sector > 0x3FFFFF || lseek(fd, off, SEEK_SET) != off) {
		return -1;
	}
	const uint8_t *p = buf;
	while (len) {
		long n = write(fd, p, len > 65536 ? 65536 : len);
		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (uint32_t)n;
	}
	return 0;
}

/* Formats COUNT sectors of the device open as FD from START, with
 * NFILES files in the root directory. With confirm, describes what it is
 * about to do as DEV and asks first. 0, or 1 after a message. */
static int mkfat_format(int fd, const char *dev, uint32_t start, uint32_t count,
                        const char *label, struct file_in *files, int nfiles, int confirm)
{
	const char *path = dev;
	/* The smallest cluster size that keeps the count FAT16's. */
	uint32_t spc = 1, fat_sectors = 0, clusters = 0;
	for (; spc <= 64; spc *= 2) {
		fat_sectors = 1;
		for (int i = 0; i < 8; i++) {
			uint32_t data = count - 1 - 2 * fat_sectors - ROOT_SECTORS;
			clusters = count > 1 + 2 * fat_sectors + ROOT_SECTORS ? data / spc : 0;
			fat_sectors = ((clusters + 2) * 2 + SECTOR - 1) / SECTOR;
		}
		if (clusters <= MAX_CLUSTERS) {
			break;
		}
	}
	if (clusters < MIN_CLUSTERS || clusters > MAX_CLUSTERS) {
		return mkfat_fail("FAT16 needs 2 MiB to 2 GiB", 0);
	}
	uint32_t data_start = 1 + 2 * fat_sectors + ROOT_SECTORS;
	uint32_t cluster_bytes = spc * SECTOR, next = 2;
	for (int i = 0; i < nfiles; i++) {
		next += (files[i].size + cluster_bytes - 1) / cluster_bytes;
	}
	if (nfiles > ROOT_ENTRIES - 1) {
		return mkfat_fail("too many files for the root directory", 0);
	}
	if (next - 2 > clusters) {
		return mkfat_fail("the files do not fit", 0);
	}

	printf("mkfat: FAT16 on %s, sectors %lu-%lu (%lu MiB), %lu clusters of %lu bytes\n", path,
	       (unsigned long)start, (unsigned long)(start + count - 1),
	       (unsigned long)(count / 2048), (unsigned long)clusters, (unsigned long)cluster_bytes);
	if (confirm) {
		char line[16];
		printf("Everything there will be lost. Type yes to go on: ");
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin) || strncmp(line, "yes", 3)) {
			printf("Nothing was written.\n");
			return 1;
		}
	}

	/* The boot sector. */
	uint8_t *s = calloc(1, SECTOR);
	uint8_t *fat = calloc(fat_sectors, SECTOR);
	uint8_t *root = calloc(ROOT_SECTORS, SECTOR);
	if (!s || !fat || !root) {
		return mkfat_fail("out of memory", 0);
	}
	s[0] = 0xEB, s[1] = 0x3C, s[2] = 0x90;
	memcpy(s + 3, "MANIOS  ", 8);
	put16(s + 11, SECTOR);
	s[13] = (uint8_t)spc;
	put16(s + 14, 1);          /* reserved sectors */
	s[16] = 2;                 /* FATs */
	put16(s + 17, ROOT_ENTRIES);
	if (count < 65536) {
		put16(s + 19, count);
	} else {
		put32(s + 32, count);
	}
	s[21] = 0xF8;              /* a fixed disk */
	put16(s + 22, fat_sectors);
	put16(s + 24, 63);         /* sectors per track, heads: nominal */
	put16(s + 26, 255);
	put32(s + 28, start);      /* hidden sectors: where the partition starts */
	s[36] = 0x80;
	s[38] = 0x29;
	put32(s + 39, 0x4D414E49u ^ start ^ count); /* a volume serial number */
	char vol[11];
	memset(vol, ' ', 11);
	for (int i = 0; i < 11 && label[i]; i++) {
		vol[i] = label[i] >= 'a' && label[i] <= 'z' ? (char)(label[i] - 32) : label[i];
	}
	memcpy(s + 43, vol, 11);
	memcpy(s + 54, "FAT16   ", 8);
	s[510] = 0x55, s[511] = 0xAA;

	/* The FAT: media byte, end-of-chain, then each file's clusters. */
	put16(fat, 0xFFF8);
	put16(fat + 2, 0xFFFF);
	memcpy(root, vol, 11);
	root[11] = 0x08;           /* the volume label */
	uint32_t c = 2;
	for (int i = 0; i < nfiles; i++) {
		uint8_t *e = root + 32 * (i + 1);
		uint32_t n = (files[i].size + cluster_bytes - 1) / cluster_bytes;
		memcpy(e, files[i].name83, 11);
		e[11] = 0x20;          /* archive */
		put16(e + 24, (1 << 5) | 1 | ((2026 - 1980) << 9)); /* 2026-01-01 */
		put16(e + 18, (1 << 5) | 1 | ((2026 - 1980) << 9));
		put16(e + 26, n ? c : 0);
		put32(e + 28, files[i].size);
		for (uint32_t k = 0; k < n; k++, c++) {
			put16(fat + 2 * c, k + 1 < n ? c + 1 : 0xFFFF);
		}
	}

	/* The boot sector last, so a half-made volume does not mount. */
	uint8_t *zero = calloc(1, SECTOR);
	if (mkfat_write(fd, start, zero, SECTOR) < 0
	    || mkfat_write(fd, start + 1, fat, fat_sectors * SECTOR) < 0
	    || mkfat_write(fd, start + 1 + fat_sectors, fat, fat_sectors * SECTOR) < 0
	    || mkfat_write(fd, start + 1 + 2 * fat_sectors, root, ROOT_SECTORS * SECTOR) < 0) {
		return mkfat_fail("writing failed", strerror(errno));
	}
	c = 2;
	for (int i = 0; i < nfiles; i++) {
		uint32_t n = (files[i].size + cluster_bytes - 1) / cluster_bytes;
		uint8_t *buf = calloc(n ? n : 1, cluster_bytes);
		if (!buf) {
			return mkfat_fail("out of memory", 0);
		}
		memcpy(buf, files[i].data, files[i].size);
		if (n && mkfat_write(fd, start + data_start + (c - 2) * spc, buf, n * cluster_bytes) < 0) {
			return mkfat_fail("writing failed", strerror(errno));
		}
		free(buf);
		c += n;
	}
	if (mkfat_write(fd, start, s, SECTOR) < 0) {
		return mkfat_fail("writing failed", strerror(errno));
	}
	free(s);
	free(fat);
	free(root);
	free(zero);
	return 0;
}

#ifndef MKFAT_LIBRARY
int main(int argc, char **argv)
{
	int yes = 0, a = 1, nfiles = 0;
	const char *label = "MANIOS";
	struct file_in files[FILES_MAX];
	for (; a < argc && argv[a][0] == '-'; a++) {
		if (!strcmp(argv[a], "-y")) {
			yes = 1;
		} else if (!strcmp(argv[a], "-l") && a + 1 < argc) {
			label = argv[++a];
		} else if (!strcmp(argv[a], "-f") && a + 1 < argc && nfiles < FILES_MAX) {
			char *eq = strchr(argv[++a], '=');
			struct file_in *f = &files[nfiles];
			if (!eq) {
				return mkfat_fail("-f wants NAME=FILE", argv[a]);
			}
			*eq = 0;
			if (name83(argv[a], f->name83) < 0) {
				return mkfat_fail("not an 8.3 name", argv[a]);
			}
			int fd = open(eq + 1, OREAD);
			struct zkt_dirent st;
			if (fd < 0 || fstat(fd, &st) < 0 || st.size > 1 << 20) {
				return mkfat_fail(eq + 1, fd < 0 ? strerror(errno) : "too big (1 MiB at most)");
			}
			f->size = st.size;
			f->data = malloc(st.size ? st.size : 1);
			if (!f->data || read(fd, f->data, st.size) != (long)st.size) {
				return mkfat_fail(eq + 1, "cannot read it");
			}
			close(fd);
			nfiles++;
		} else {
			break;
		}
	}
	if (argc - a != 1 && argc - a != 3) {
		fprintf(stderr, "usage: mkfat [-y] [-l LABEL] [-f NAME=FILE]... DEVICE [START COUNT]\n");
		return 2;
	}
	char path[40];
	const char *dev = argv[a];
	snprintf(path, sizeof(path), "%s%s", strncmp(dev, "/dev/", 5) ? "/dev/" : "", dev);
	int fd = open(path, ORDWR);
	struct zkt_dirent st;
	if (fd < 0 || fstat(fd, &st) < 0) {
		return mkfat_fail(path, strerror(errno));
	}
	if (st.type != ZKT_TYPE_DEVICE) {
		return mkfat_fail("not a device", path);
	}
	uint32_t start = 0, count = st.size / SECTOR;
	if (argc - a == 3) {
		start = (uint32_t)strtoul(argv[a + 1], 0, 0);
		count = (uint32_t)strtoul(argv[a + 2], 0, 0);
		if (start >= st.size / SECTOR || count > st.size / SECTOR - start) {
			return mkfat_fail("START COUNT runs past the end of the device", path);
		}
	}

	int rc = mkfat_format(fd, path, start, count, label, files, nfiles, !yes);
	close(fd);
	if (!rc) {
		printf("mkfat: done\n");
	}
	return rc;
}
#endif
