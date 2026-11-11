#ifndef XBOX_DIAG_H
#define XBOX_DIAG_H
#include <stdio.h>
/* Slow-call diagnostics, active only with D:\DEBUG.TXT (see xbox_entry.c). */
typedef struct { unsigned t0; const char *prev; } xb_diag;
void   xb_diag_begin(xb_diag *d, const char *mark);
void   xb_diag_end(xb_diag *d);
FILE  *xb_fopen(const char *path, const char *mode);
/* xbox_fcache.c: read-ahead cache for files opened with xb_fopen */
void   xb_fcache_attach(FILE *f, const char *mode, const char *name);
size_t xb_fread(void *buf, size_t size, size_t count, FILE *f);
int    xb_fseek(FILE *f, long off, int whence);
long   xb_ftell(FILE *f);
int    xb_fclose(FILE *f);
#endif
