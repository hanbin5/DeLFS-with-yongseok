/* Small, static guest payload. All paths are relative to the test mount. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *op)
{
	perror(op);
	exit(1);
}

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "recovery mismatch: %s\n", what);
		exit(1);
	}
}

static int checked(int rc, const char *op)
{
	if (rc < 0)
		fail(op);
	return rc;
}

static void syncdir(const char *name)
{
	int fd = checked(open(name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW), name);
	checked(fsync(fd), name);
	checked(close(fd), name);
}

static void pattern(unsigned char *b, size_t size, unsigned seed)
{
	for (size_t i = 0; i < size; ++i) {
		seed = seed * 1664525u + 1013904223u;
		b[i] = seed >> 24;
	}
}

#define PAYLOAD (2 * 4096 + 137)
static void put(int fd, size_t size, unsigned seed)
{
	unsigned char b[PAYLOAD];
	pattern(b, size, seed);
	require(checked(write(fd, b, size), "write") == (int)size, "short write");
}

static void verify(const char *name, size_t size, unsigned seed)
{
	unsigned char want[PAYLOAD], got[PAYLOAD];
	int fd = checked(open(name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK), name);
	struct stat st;
	checked(fstat(fd, &st), name);
	require(S_ISREG(st.st_mode) && st.st_size == (off_t)size, "type/size");
	pattern(want, size, seed);
	require(checked(read(fd, got, size), name) == (int)size &&
		!memcmp(want, got, size), "file bytes");
	checked(close(fd), name);
}

static void entries(const char *dir, const char *a, const char *b)
{
	DIR *d = fdopendir(checked(open(dir, O_DIRECTORY | O_RDONLY | O_NOFOLLOW), dir));
	struct dirent *e;
	if (!d)
		fail(dir);
	errno = 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (a && !strcmp(e->d_name, a))
			a = NULL;
		else if (b && !strcmp(e->d_name, b))
			b = NULL;
		else
			require(0, e->d_name);
	}
	if (errno)
		fail(dir);
	require(!a && !b, "missing directory entry");
	checked(closedir(d), dir);
}

static void tree(int create, const char *label)
{
	unsigned seed = !strcmp(label, "cut") ? 123 : 567;
	require(!strcmp(label, "cut") || !strcmp(label, "clean"), "label");
	if (create) {
		checked(mkdir(label, 0700), label);
		syncdir(".");
		checked(chdir(label), label);
		checked(mkdir("src", 0700), "src");
		checked(mkdir("dst", 0700), "dst");
		syncdir(".");
		int fd = checked(open("src/staging", O_WRONLY | O_CREAT | O_EXCL, 0600), "create");
		put(fd, PAYLOAD, seed);
		checked(fsync(fd), "file fsync");
		checked(close(fd), "close");
		syncdir("src");
		checked(rename("src/staging", "dst/published"), "rename");
		syncdir("src");
		syncdir("dst");
	} else {
		int fd = checked(open(label, O_DIRECTORY | O_RDONLY | O_NOFOLLOW), label);
		checked(fchdir(fd), label);
		checked(close(fd), label);
		entries(".", "src", "dst");
		entries("src", NULL, NULL);
		entries("dst", "published", NULL);
		verify("dst/published", PAYLOAD, seed);
	}
}

static void rotate(int generation)
{
	int root = checked(open(".", O_DIRECTORY | O_RDONLY), ".");
	if (generation)
		verify("generation", 4096, generation - 1);
	int fd = checked(open("generation", O_WRONLY | O_NOFOLLOW |
		(generation ? 0 : O_CREAT | O_EXCL), 0600), "generation");
	put(fd, 4096, generation);
	checked(close(fd), "generation");
	checked(syncfs(root), "generation syncfs");
	checked(close(root), ".");
}

#define ORPHAN_BLOCKS 16
static void orphan(const char *token)
{
	struct stat st;
	int root = checked(open(".", O_DIRECTORY | O_RDONLY), ".");
	int fd = checked(open("orphan", O_RDWR | O_CREAT | O_EXCL, 0600), "orphan");
	for (int i = 0; i < ORPHAN_BLOCKS; ++i)
		put(fd, 4096, 800 + i);
	checked(fsync(fd), "orphan fsync");
	checked(unlink("orphan"), "unlink");
	checked(fsync(root), "orphan parent fsync");
	checked(syncfs(root), "orphan checkpoint");
	checked(fstat(fd, &st), "unlinked fd");
	require(!st.st_nlink && st.st_size == ORPHAN_BLOCKS * 4096, "held orphan");
	printf("PR:%s:orphan:%" PRIuMAX "\n", token, (uintmax_t)st.st_ino);
	fflush(stdout);
	for (;;)
		pause(); /* The descriptor must not close until QEMU itself is killed. */
}

int main(int argc, char **argv)
{
	if (argc == 3 && (!strcmp(argv[1], "put") || !strcmp(argv[1], "get")))
		tree(!strcmp(argv[1], "put"), argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "rotate") &&
		 strlen(argv[2]) == 1 && strchr("012", argv[2][0]))
		rotate(argv[2][0] - '0');
	else if (argc == 2 && !strcmp(argv[1], "generation2"))
		verify("generation", 4096, 2);
	else if (argc == 3 && !strcmp(argv[1], "orphan")) {
		orphan(argv[2]);
	} else if (argc == 2 && !strcmp(argv[1], "reclaimed")) {
		struct stat st;
		verify("generation", 4096, 2);
		require(lstat("orphan", &st) < 0 && errno == ENOENT, "orphan name remains");
	} else
		return 2;
	return 0;
}
