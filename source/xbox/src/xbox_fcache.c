/*
 * xbox_fcache.c - read-ahead cache for the game's file reads (fOpen/fRead/...).
 *
 * The 1998 loaders read textures one colour byte at a time (texture.c does
 * three fRead(&x, 1, 1, fp) per pixel), i.e. about a million tiny fread()
 * calls for one menu picture. On the Xbox's libc each call costs several
 * microseconds, which showed up as 5-8 second freezes on every menu change and
 * race start (logged as "fread/fclose BIN\OPTION\SMODE00.RAW" by the watchdog).
 * Here a read-only file gets a 64 KB buffer, so those calls become a memcpy.
 *
 * Semantics kept: file position (ftell/fseek), short reads at the end, and the
 * EOF flag - which is only set when a read really goes past the end of the
 * file, so feof() on these FILEs behaves as before.
 * Only files opened by xb_fopen() for plain reading ("r"/"rb") are cached; all
 * other FILEs pass straight through. Not thread-safe: used from the game's
 * main thread, like the loaders it serves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_diag.h"

#define FC_BUF   (64 * 1024)
#define FC_FILES 16

typedef struct {
    FILE          *f;
    unsigned char *buf;
    long           bufStart;   /* file offset of buf[0]; real position = bufStart + bufLen */
    size_t         bufLen;
    size_t         bufPos;
    long           size;
    char           mark[96];
} fc_t;

static fc_t s_fc[FC_FILES];

static fc_t *fc_find(FILE *f)
{
    for (int i = 0; i < FC_FILES; i++)
        if (s_fc[i].f == f) return &s_fc[i];
    return NULL;
}

void xb_fcache_attach(FILE *f, const char *mode, const char *name)
{
    if (!f || !mode || mode[0] != 'r' || strchr(mode, '+')) return;
    fc_t *e = NULL;
    for (int i = 0; i < FC_FILES; i++)
        if (!s_fc[i].f) { e = &s_fc[i]; break; }
    if (!e) return;
    long pos = ftell(f);
    if (pos < 0 || fseek(f, 0, SEEK_END) != 0) return;
    long size = ftell(f);
    if (size < 0 || fseek(f, pos, SEEK_SET) != 0) return;
    unsigned char *b = (unsigned char *)malloc(FC_BUF);
    if (!b) return;
    e->f = f;
    e->buf = b;
    e->bufStart = pos;
    e->bufLen = 0;
    e->bufPos = 0;
    e->size = size;
    snprintf(e->mark, sizeof(e->mark), "read %s", name ? name : "?");
}

static size_t fc_refill(fc_t *e)
{
    long real = e->bufStart + (long)e->bufLen;   /* real file position */
    long left = e->size - real;
    e->bufStart = real;
    e->bufLen = 0;
    e->bufPos = 0;
    if (left <= 0) {
        unsigned char c;
        (void)fread(&c, 1, 1, e->f);   /* really at the end: sets the EOF flag */
        return 0;
    }
    size_t want = left < FC_BUF ? (size_t)left : (size_t)FC_BUF;
    xb_diag d;
    xb_diag_begin(&d, e->mark);
    size_t got = fread(e->buf, 1, want, e->f);
    xb_diag_end(&d);
    e->bufLen = got;
    return got;
}

size_t xb_fread(void *dst, size_t size, size_t count, FILE *f)
{
    fc_t *e = fc_find(f);
    if (!e) return fread(dst, size, count, f);
    size_t n = size * count;
    if (n == 0) return 0;
    unsigned char *out = (unsigned char *)dst;
    size_t done = 0;
    while (done < n) {
        if (e->bufPos >= e->bufLen) {
            if (n - done >= FC_BUF) {
                /* big read: straight into the caller's buffer */
                size_t rem = n - done;
                long real = e->bufStart + (long)e->bufLen;
                long left = e->size - real;
                size_t want = (left > 0 && (size_t)left < rem) ? (size_t)left : (left > 0 ? rem : 0);
                size_t got = 0;
                if (want) {
                    xb_diag d;
                    xb_diag_begin(&d, e->mark);
                    got = fread(out + done, 1, want, f);
                    xb_diag_end(&d);
                }
                e->bufStart = real + (long)got;
                e->bufLen = 0;
                e->bufPos = 0;
                done += got;
                if (got < rem) {
                    if (want < rem) {          /* asked for more than the file holds: EOF */
                        unsigned char c;
                        (void)fread(&c, 1, 1, f);
                    }
                    break;
                }
                continue;
            }
            if (fc_refill(e) == 0) break;
        }
        size_t take = e->bufLen - e->bufPos;
        if (take > n - done) take = n - done;
        memcpy(out + done, e->buf + e->bufPos, take);
        e->bufPos += take;
        done += take;
    }
    return done / size;
}

int xb_fseek(FILE *f, long off, int whence)
{
    fc_t *e = fc_find(f);
    if (!e) return fseek(f, off, whence);
    long lpos = e->bufStart + (long)e->bufPos;
    long target;
    if (whence == SEEK_SET)      target = off;
    else if (whence == SEEK_CUR) target = lpos + off;
    else if (whence == SEEK_END) target = e->size + off;
    else return -1;
    if (target < 0) return -1;
    clearerr(f);
    if (target >= e->bufStart && target <= e->bufStart + (long)e->bufLen) {
        e->bufPos = (size_t)(target - e->bufStart);
        return 0;
    }
    if (fseek(f, target, SEEK_SET) != 0) return -1;
    e->bufStart = target;
    e->bufLen = 0;
    e->bufPos = 0;
    return 0;
}

long xb_ftell(FILE *f)
{
    fc_t *e = fc_find(f);
    if (!e) return ftell(f);
    return e->bufStart + (long)e->bufPos;
}

int xb_fclose(FILE *f)
{
    fc_t *e = fc_find(f);
    if (e) {
        free(e->buf);
        memset(e, 0, sizeof(*e));
    }
    return fclose(f);
}
