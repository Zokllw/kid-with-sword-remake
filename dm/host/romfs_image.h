// DMROMFS1's index loader, shared by the host mount and iOS startup. Startup must validate the same index before replacing a working folder.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint64_t a, b; uint32_t name; uint16_t nlen; uint8_t type, flags; } __attribute__((packed)) img_ent;   // directory: a = first child, b = children; file: a = offset, b = size
_Static_assert(sizeof(img_ent) == 24, "RomFS image entry layout");
#define IMG_ENT_DAMAGED 1    // in the image: this file failed the NCA's hash check and was written as read (nx_extract --allow-damaged)
#define IMG_ENT_REPORTED 2   // in memory only: the host has named this damaged file in its log
// An image in parts (romfs_image_open_parts): <path>.0, <path>.1, ... of psize bytes each but the last, read as one; fd is part 0's. Only the shader libraries' image is ever in parts
// (pack_ipa.py: an IPA entry over 4 GiB needs ZIP64, which on-device signers' unzippers may not read), and only romfs_image_pread reads across them
#define ROMFS_PARTS_MAX 16
typedef struct { int fd; uint64_t n, damaged; img_ent *e; const char *names; int nparts, pfd[ROMFS_PARTS_MAX]; uint64_t psize; } romfs_img;

static void romfs_image_close(romfs_img *m) {
    if (!m) return;
    if (m->nparts) { for (int i = 0; i < m->nparts; i++) close(m->pfd[i]); } else close(m->fd);
    free(m->e); free(m);
}
static int romfs_image_read(int fd, void *buf, size_t size, off_t off) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = pread(fd, (char *)buf + got, size - got, off + (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EINVAL; return 0; }
        got += (size_t)n;
    }
    return 1;
}
// A read of the image at off, across its parts when it has them
__attribute__((unused)) static int romfs_image_pread(const romfs_img *m, void *buf, size_t size, off_t off) {
    if (!m->nparts) return romfs_image_read(m->fd, buf, size, off);
    while (size) {
        uint64_t i = (uint64_t)off / m->psize, at = (uint64_t)off % m->psize;
        if (i >= (uint64_t)m->nparts) { errno = EINVAL; return 0; }
        size_t n = i + 1 < (uint64_t)m->nparts && size > m->psize - at ? (size_t)(m->psize - at) : size;
        if (!romfs_image_read(m->pfd[i], buf, n, (off_t)at)) return 0;
        buf = (char *)buf + n; size -= n; off += (off_t)n;
    }
    return 1;
}
static romfs_img *romfs_image_load(romfs_img *m, uint64_t total);
// NULL for an incomplete or invalid image (errno EINVAL), an I/O error, or allocation failure. The caller owns the descriptor and index on success.
static romfs_img *romfs_image_open(const char *path) {
    int fd = open(path, O_RDONLY); struct stat st; romfs_img *m;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !(m = calloc(1, sizeof *m))) { int e = errno; close(fd); errno = e; return NULL; }
    m->fd = fd;
    return romfs_image_load(m, (uint64_t)st.st_size);
}
// The image at path, or else in parts <path>.0, <path>.1, ...: each part but the last of the same size, the last no bigger; NULL as romfs_image_open (ENOENT: neither is there)
__attribute__((unused)) static romfs_img *romfs_image_open_parts(const char *path) {
    romfs_img *m = romfs_image_open(path);
    if (m || errno != ENOENT) return m;
    if (!(m = calloc(1, sizeof *m))) return NULL;
    char p[4096]; uint64_t total = 0; struct stat st;
    for (int i = 0; i < ROMFS_PARTS_MAX; i++) {
        if (snprintf(p, sizeof p, "%s.%d", path, i) >= (int)sizeof p) break;
        int fd = open(p, O_RDONLY); if (fd < 0) break;
        m->pfd[m->nparts++] = fd;
        if (fstat(fd, &st) || !st.st_size) { errno = EINVAL; goto bad; }
        if (i && (uint64_t)st.st_size > m->psize) { errno = EINVAL; goto bad; }   // no part bigger than the first (the parts before this one were each its size)
        if (!i) m->psize = (uint64_t)st.st_size;
        total += (uint64_t)st.st_size;
        if ((uint64_t)st.st_size < m->psize) break;   // a shorter part is the last
    }
    if (!m->nparts) { free(m); errno = ENOENT; return NULL; }
    m->fd = m->pfd[0];
    return romfs_image_load(m, total);
bad: { int e = errno; romfs_image_close(m); errno = e; return NULL; }
}
// Validates the image whose descriptors m holds (total bytes) and reads its index; frees m and returns NULL when it is not one
static romfs_img *romfs_image_load(romfs_img *m, uint64_t total) {
    uint8_t h[32]; uint64_t ioff, isz, n;
    if (!romfs_image_pread(m, h, sizeof h, 0)) goto bad;
    errno = EINVAL;
    if (memcmp(h, "DMROMFS1", 8) || total < sizeof h) goto bad;
    memcpy(&ioff, h + 8, 8); memcpy(&isz, h + 16, 8); memcpy(&n, h + 24, 8);
    if (!n || n > UINT32_MAX || ioff < sizeof h || ioff > total || isz != total - ioff || isz > SIZE_MAX || n > isz / sizeof(img_ent)) goto bad;
    if (!(m->e = malloc((size_t)isz)) || !romfs_image_pread(m, m->e, (size_t)isz, (off_t)ioff)) goto bad;
    m->n = n; m->names = (const char *)m->e + n * sizeof(img_ent);
    uint64_t pool = isz - n * sizeof(img_ent), next_child = 1;
    errno = EINVAL;
    if (m->e[0].type || m->e[0].nlen) goto bad;
    for (uint64_t k = 0; k < n; k++) {
        const img_ent *x = &m->e[k];
        if ((uint64_t)x->name + x->nlen > pool || x->type > 1 || x->flags > (x->type ? IMG_ENT_DAMAGED : 0)) goto bad;
        m->damaged += x->flags;
        const char *name = m->names + x->name;
        if (k && (!x->nlen || x->nlen > 0x300 || memchr(name, 0, x->nlen) || memchr(name, '/', x->nlen) || (x->nlen == 1 && name[0] == '.') || (x->nlen == 2 && !memcmp(name, "..", 2)))) goto bad;
        if (x->type) { if (x->a < sizeof h || x->a > ioff || x->b > ioff - x->a) goto bad; }
        else {
            // Breadth-first child runs partition entries 1..n-1; reject cycles, aliases and unreachable entries.
            if (x->a != next_child || x->a <= k || x->a > n || x->b > n - x->a) goto bad;
            next_child += x->b;
        }
    }
    if (next_child != n) goto bad;
    for (uint64_t k = 0; k < n; k++) {
        const img_ent *d = &m->e[k]; if (d->type) continue;
        for (uint64_t j = 1; j < d->b; j++) {
            const img_ent *a = &m->e[d->a + j - 1], *b = a + 1;
            size_t len = a->nlen < b->nlen ? a->nlen : b->nlen;
            int c = memcmp(m->names + a->name, m->names + b->name, len);
            if (c > 0 || (!c && a->nlen >= b->nlen)) goto bad;
        }
    }
    return m;
bad: {
    int e = errno ? errno : EINVAL;
    romfs_image_close(m);
    errno = e; return NULL;
    }
}
