/* sonicr_music — prepares the Sonic R soundtrack for the Xbox port.
 *
 * The PC game (1998) plays its music as CD audio tracks, which are not files.
 * The Xbox port plays MUSIC\trackN.son: headerless 44100 Hz, 16-bit, stereo,
 * little-endian PCM — the exact layout of CD audio. This tool produces those
 * files two ways:
 *
 *   1. Straight from the original Sonic R CD (Windows): every audio track is
 *      read digitally from the disc and written as MUSIC\trackN.son, where N
 *      is the CD track number (the numbers the game asks for).
 *   2. From WAV files you already ripped: any PCM or float WAV (8/16/24/32-bit,
 *      mono or stereo, any sample rate) is converted to 44100 Hz 16-bit
 *      stereo. The track number is taken from the file name ("track02.wav",
 *      "Sonic R - 05.wav", "07 Can You Feel the Sunshine.wav" ...).
 *
 * Usage:
 *   sonicr_music                 rip from the first CD drive with audio tracks
 *   sonicr_music E:              rip from drive E:
 *   sonicr_music file.wav ...    convert WAV files (or drag them onto the exe)
 *   sonicr_music folder          convert every .wav in a folder
 *   -o DIR                       output folder (default: MUSIC next to the tool
 *                                when ripping, MUSIC next to the WAVs otherwise)
 *
 * Copy the MUSIC folder next to default.xbe on the Xbox.
 *
 * Builds as a 32-bit Windows console program (works on Windows XP..11); the
 * WAV conversion part also builds on Linux/macOS with any C compiler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <strings.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <ntddscsi.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define SEP '\\'
#else
#include <sys/stat.h>
#include <dirent.h>
#define MKDIR(p) mkdir(p, 0755)
#define SEP '/'
#endif

static int g_pause = 0;   /* keep the console window open when started by double click / drag */

static void finish(int code)
{
    if (g_pause) {
        printf("\nPress Enter to close.\n");
        getchar();
    }
    exit(code);
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && (dir[l - 1] == '\\' || dir[l - 1] == '/'))
        snprintf(out, n, "%s%s", dir, name);
    else
        snprintf(out, n, "%s%c%s", dir, SEP, name);
}

/* ------------------------------------------------------------------ */
/* WAV -> .son                                                         */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Track number from a file name: the first run of digits, 1..99. */
static int track_from_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    for (const char *p = base; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            int n = atoi(p);
            if (n >= 1 && n <= 99) return n;
            return -1;
        }
    }
    return -1;
}

static float sample_at(const uint8_t *d, int fmt, int bits, int idx)
{
    switch (bits) {
    case 8:  return ((int)d[idx] - 128) / 128.0f;
    case 16: return (int16_t)rd16(d + idx * 2) / 32768.0f;
    case 24: {
        int32_t v = d[idx * 3] | (d[idx * 3 + 1] << 8) | (d[idx * 3 + 2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF;
        return v / 8388608.0f;
    }
    case 32:
        if (fmt == 3) { float f; memcpy(&f, d + idx * 4, 4); return f; }
        return (int32_t)rd32(d + idx * 4) / 2147483648.0f;
    }
    return 0.0f;
}

static int16_t to16(float f)
{
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int16_t)(f * 32767.0f);
}

static int convert_wav(const char *in, const char *outDir)
{
    int track = track_from_name(in);
    if (track < 0) {
        printf("  %s: no track number in the file name - skipped\n", in);
        return 0;
    }
    FILE *f = fopen(in, "rb");
    if (!f) { printf("  %s: cannot open\n", in); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        printf("  %s: read error\n", in); fclose(f); free(buf); return 0;
    }
    fclose(f);
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        printf("  %s: not a WAV file (convert it to WAV first) - skipped\n", in);
        free(buf); return 0;
    }
    int fmt = 0, ch = 0, bits = 0; uint32_t rate = 0;
    const uint8_t *data = NULL; uint32_t dataLen = 0;
    for (long p = 12; p + 8 <= size; ) {
        uint32_t len = rd32(buf + p + 4);
        if (!memcmp(buf + p, "fmt ", 4) && len >= 16) {
            fmt = rd16(buf + p + 8); ch = rd16(buf + p + 10);
            rate = rd32(buf + p + 12); bits = rd16(buf + p + 22);
            if (fmt == 0xFFFE && len >= 40) fmt = rd16(buf + p + 32);   /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(buf + p, "data", 4)) {
            data = buf + p + 8;
            dataLen = len;
            if ((long)(p + 8 + dataLen) > size) dataLen = (uint32_t)(size - p - 8);
        }
        p += 8 + len + (len & 1);
    }
    if (!data || (fmt != 1 && fmt != 3) || ch < 1 || ch > 2 || rate < 8000 ||
        !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        printf("  %s: unsupported WAV (need PCM/float, mono/stereo) - skipped\n", in);
        free(buf); return 0;
    }
    int frameBytes = ch * bits / 8;
    long frames = (long)(dataLen / frameBytes);
    long outFrames = (long)((double)frames * 44100.0 / rate);
    char name[32], out[1024];
    snprintf(name, sizeof(name), "track%d.son", track);
    join(out, sizeof(out), outDir, name);
    FILE *o = fopen(out, "wb");
    if (!o) { printf("  cannot write %s\n", out); free(buf); return 0; }
    int16_t blk[2 * 4096];
    int n = 0;
    for (long i = 0; i < outFrames; i++) {
        double src = (double)i * rate / 44100.0;
        long s0 = (long)src;
        long s1 = s0 + 1 < frames ? s0 + 1 : s0;
        float t = (float)(src - s0);
        for (int c = 0; c < 2; c++) {
            int cc = ch == 2 ? c : 0;
            float a = sample_at(data, fmt, bits, (int)(s0 * ch + cc));
            float b = sample_at(data, fmt, bits, (int)(s1 * ch + cc));
            blk[n++] = to16(a + (b - a) * t);
        }
        if (n == (int)(sizeof(blk) / sizeof(blk[0]))) {
            fwrite(blk, 2, (size_t)n, o);
            n = 0;
        }
    }
    if (n) fwrite(blk, 2, (size_t)n, o);
    fclose(o);
    free(buf);
    printf("  %s -> %s (%ld:%02ld)\n", in, out, outFrames / 44100 / 60, (outFrames / 44100) % 60);
    return 1;
}

static int has_wav_ext(const char *s)
{
    size_t l = strlen(s);
    return l > 4 && (s[l - 4] == '.') && tolower((unsigned char)s[l - 3]) == 'w' &&
           tolower((unsigned char)s[l - 2]) == 'a' && tolower((unsigned char)s[l - 1]) == 'v';
}

static int convert_folder(const char *dir, const char *outDir)
{
    int done = 0;
#ifdef _WIN32
    char pat[1024];
    WIN32_FIND_DATAA fd;
    join(pat, sizeof(pat), dir, "*.wav");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char p[1024];
        join(p, sizeof(p), dir, fd.cFileName);
        done += convert_wav(p, outDir);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_wav_ext(e->d_name)) continue;
        char p[1024];
        join(p, sizeof(p), dir, e->d_name);
        done += convert_wav(p, outDir);
    }
    closedir(d);
#endif
    return done;
}

static int is_dir(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* ------------------------------------------------------------------ */
/* CD audio ripping (Windows)                                          */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#define SECT_RAW 2352
#define READ_SECTS 20

#pragma pack(push, 1)
typedef struct { UCHAR Reserved, Control_Adr, TrackNumber, Reserved1; UCHAR Address[4]; } TOC_TRACK;
typedef struct { UCHAR Length[2]; UCHAR FirstTrack, LastTrack; TOC_TRACK Tracks[100]; } TOC_DATA;
typedef struct { LARGE_INTEGER DiskOffset; ULONG SectorCount; ULONG TrackMode; } RAW_INFO;
#pragma pack(pop)
#define IOCTL_READ_TOC 0x00024000      /* IOCTL_CDROM_READ_TOC */
#define IOCTL_RAW_READ 0x0002403E      /* IOCTL_CDROM_RAW_READ */
#define TRACKMODE_CDDA 2

static long msf_to_lba(const UCHAR *a) { return ((long)a[1] * 60 + a[2]) * 75 + a[3] - 150; }

static HANDLE open_drive(char letter)
{
    char path[16];
    snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static int read_toc(HANDLE h, TOC_DATA *toc)
{
    DWORD got = 0;
    memset(toc, 0, sizeof(*toc));
    return DeviceIoControl(h, IOCTL_READ_TOC, NULL, 0, toc, sizeof(*toc), &got, NULL) != 0;
}

/* One audio track: its CD track number and its sectors [start, end). */
typedef struct { int number; long start, end; } AUDIO_TRACK;

/* The full TOC (every session): needed when a disc has more than one session,
 * as the plain TOC then only tells about the first one. */
#define IOCTL_READ_TOC_EX 0x00024054   /* IOCTL_CDROM_READ_TOC_EX */
#pragma pack(push, 1)
typedef struct { UCHAR Format; UCHAR SessionTrack; UCHAR Reserved2, Reserved3; } TOC_EX_IN;
typedef struct { UCHAR Session, Control_Adr, Tno, Point, MsfExtra[3], Zero, Msf[3]; } FULL_DESC;
typedef struct { UCHAR Length[2]; UCHAR FirstSession, LastSession; FULL_DESC D[200]; } FULL_TOC;
#pragma pack(pop)

static int read_full_toc(HANDLE h, FULL_TOC *ft, int *count)
{
    TOC_EX_IN in;
    DWORD got = 0;
    memset(ft, 0, sizeof(*ft));
    memset(&in, 0, sizeof(in));
    in.Format = 2;   /* CDROM_READ_TOC_EX_FORMAT_FULL_TOC */
    if (!DeviceIoControl(h, IOCTL_READ_TOC_EX, &in, sizeof(in), ft, sizeof(*ft), &got, NULL) || got < 4)
        return 0;
    int len = ((ft->Length[0] << 8) | ft->Length[1]) - 2;
    *count = len > 0 ? len / (int)sizeof(FULL_DESC) : 0;
    if (*count > 200) *count = 200;
    return 1;
}

static long msf3_to_lba(const UCHAR *m) { return ((long)m[0] * 60 + m[1]) * 75 + m[2] - 150; }

/* Audio tracks of the disc: from the plain TOC, or when that shows none, from
 * the full TOC of every session. Returns the number of tracks found. */
static int list_audio_tracks(HANDLE h, AUDIO_TRACK *out, int *fromFull)
{
    TOC_DATA toc;
    int n = 0;
    *fromFull = 0;
    if (read_toc(h, &toc)) {
        int tracks = toc.LastTrack - toc.FirstTrack + 1;
        for (int i = 0; i < tracks && n < 100; i++) {
            if (toc.Tracks[i].Control_Adr & 0x04) continue;   /* data track */
            long st = msf_to_lba(toc.Tracks[i].Address);
            long en = msf_to_lba(toc.Tracks[i + 1].Address);   /* next track or lead-out */
            if (en <= st) continue;
            out[n].number = toc.Tracks[i].TrackNumber; out[n].start = st; out[n].end = en; n++;
        }
    }
    if (n == 0) {
        static FULL_TOC ft;
        int cnt = 0;
        if (read_full_toc(h, &ft, &cnt)) {
            for (int i = 0; i < cnt && n < 100; i++) {
                const FULL_DESC *d = &ft.D[i];
                if (d->Point < 1 || d->Point > 99 || (d->Control_Adr & 0x04)) continue;
                long st = msf3_to_lba(d->Msf), en = -1;
                /* its end: the next track of the same session, else that session's lead-out (A2) */
                for (int k = 0; k < cnt; k++) {
                    const FULL_DESC *e = &ft.D[k];
                    if (e->Session != d->Session) continue;
                    long pos = msf3_to_lba(e->Msf);
                    if (((e->Point >= 1 && e->Point <= 99 && e->Point != d->Point) || e->Point == 0xA2) &&
                        pos > st && (en < 0 || pos < en)) en = pos;
                }
                if (en <= st) continue;
                out[n].number = d->Point; out[n].start = st; out[n].end = en; n++;
                *fromFull = 1;
            }
        }
    }
    return n;
}


/* ---- raw sector probe -------------------------------------------------
 * A copy of a mixed-mode disc can have everything, audio too, inside ONE data
 * track: the file system then only covers the first part (here ~105 MB of a
 * ~590 MB track) and the rest is the audio, stored as raw sectors. READ CD
 * returns a sector's 2352 bytes as stored, whatever the track type says. */
typedef struct { SCSI_PASS_THROUGH_DIRECT p; UCHAR sense[32]; } SPTD;

static HANDLE open_drive_rw(char letter)
{
    char path[16];
    snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

/* READ CD (0xBE) of one sector: `type` = expected sector type (0 any, 1 CD-DA,
 * 2 mode 1, 3 mode 2), `flags` = what to return (0x10 user data, 0xF8 everything
 * incl. sync/headers/EDC). `want` bytes come back in `out`. */
static int read_cd(HANDLE h, long lba, int type, int flags, int want, BYTE *out, UCHAR *sense3)
{
    SPTD q;
    DWORD got = 0;
    memset(&q, 0, sizeof(q));
    q.p.Length = sizeof(q.p);
    q.p.CdbLength = 12;
    q.p.SenseInfoLength = sizeof(q.sense);
    q.p.DataIn = SCSI_IOCTL_DATA_IN;
    q.p.DataTransferLength = want;
    q.p.TimeOutValue = 10;
    q.p.DataBuffer = out;
    q.p.SenseInfoOffset = offsetof(SPTD, sense);
    q.p.Cdb[0] = 0xBE;
    q.p.Cdb[1] = (UCHAR)(type << 2);
    q.p.Cdb[2] = (UCHAR)(lba >> 24); q.p.Cdb[3] = (UCHAR)(lba >> 16);
    q.p.Cdb[4] = (UCHAR)(lba >> 8);  q.p.Cdb[5] = (UCHAR)lba;
    q.p.Cdb[8] = 1;                    /* one sector */
    q.p.Cdb[9] = (UCHAR)flags;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &q, sizeof(q), &q, sizeof(q), &got, NULL))
        return -(int)GetLastError();
    if (q.p.ScsiStatus != 0) {
        if (sense3) { sense3[0] = q.sense[2] & 0x0F; sense3[1] = q.sense[12]; sense3[2] = q.sense[13]; }
        return -1000 - q.p.ScsiStatus;
    }
    return 1;
}

/* How much a sector looks like 16-bit stereo music: the average jump between
 * neighbouring samples against the average level (noise ~1.4, music well below 1). */
static double pcm_smoothness(const BYTE *b, int bytes)
{
    const short *x = (const short *)b;
    double lev = 0, dif = 0;
    for (int i = 2; i < bytes / 2; i += 2) {
        lev += abs(x[i]);
        dif += abs(x[i] - x[i - 2]);
    }
    return lev > 1000 ? dif / lev : -1.0;   /* -1: (near) silence */
}

static void probe_hidden(char letter, long leadout)
{
    static BYTE sec[SECT_RAW];
    BYTE pvd[2048];
    long volSectors = 0;
    HANDLE hv = open_drive(letter);
    HANDLE hr = open_drive_rw(letter);
    HANDLE hv2 = open_drive(letter);
    printf("Raw sector probe:\n");
    if (hv != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        LARGE_INTEGER pos; pos.QuadPart = 16 * 2048;
        SetFilePointerEx(hv, pos, NULL, FILE_BEGIN);
        if (ReadFile(hv, pvd, 2048, &got, NULL) && got == 2048 && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
            volSectors = (long)(pvd[80] | (pvd[81] << 8) | (pvd[82] << 16) | ((long)pvd[83] << 24));
            printf("  file system: %ld sectors (%ld MB); disc track: %ld sectors (%ld MB)\n",
                   volSectors, volSectors / 512, leadout, leadout / 512);
        } else printf("  file system: cannot read the volume descriptor\n");
        CloseHandle(hv);
    }
    if (hr == INVALID_HANDLE_VALUE) {
        printf("  raw reads need administrator rights (error %lu): run the program as administrator\n",
               (unsigned long)GetLastError());
        return;
    }
    long lba[3];
    long from = volSectors > 0 ? volSectors + 200 : leadout / 4;
    lba[0] = from; lba[1] = from + (leadout - 300 - from) / 2; lba[2] = leadout - 300;
    for (int i = 0; i < 3; i++) {
        /* 1. the ordinary way: 2048 bytes of user data through the file system layer */
        if (hv2 != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)lba[i] * 2048;
            SetFilePointerEx(hv2, pos, NULL, FILE_BEGIN);
            if (ReadFile(hv2, sec, 2048, &got, NULL) && got == 2048) {
                double sm = pcm_smoothness(sec, 2048);
                printf("  sector %6ld  ReadFile: ok  %02X %02X %02X %02X %02X %02X %02X %02X  smoothness %.2f%s\n",
                       lba[i], sec[0], sec[1], sec[2], sec[3], sec[4], sec[5], sec[6], sec[7], sm,
                       sm < 0 ? " (silence)" : (sm < 0.6 ? "  <- looks like music" : ""));
            } else printf("  sector %6ld  ReadFile: failed (error %lu)\n", lba[i], (unsigned long)GetLastError());
        }
        /* 2. READ CD in several ways */
        static const int tv[4][3] = { {0, 0x10, 2048}, {0, 0xF8, SECT_RAW}, {1, 0x10, SECT_RAW}, {2, 0x10, 2048} };
        for (int k = 0; k < 4; k++) {
            UCHAR sn[3] = {0, 0, 0};
            int r = read_cd(hr, lba[i], tv[k][0], tv[k][1], tv[k][2], sec, sn);
            if (r < 0) {
                printf("    READ CD type %d flags %02X: failed (%d, sense %u/%02X/%02X)\n", tv[k][0], tv[k][1], r, sn[0], sn[1], sn[2]);
            } else {
                double sm = pcm_smoothness(sec, tv[k][2]);
                printf("    READ CD type %d flags %02X: ok  %02X %02X %02X %02X %02X %02X %02X %02X  smoothness %.2f%s\n",
                       tv[k][0], tv[k][1], sec[0], sec[1], sec[2], sec[3], sec[4], sec[5], sec[6], sec[7], sm,
                       sm < 0 ? " (silence)" : (sm < 0.6 ? "  <- looks like music" : ""));
            }
        }
    }
    if (hv2 != INVALID_HANDLE_VALUE) CloseHandle(hv2);
    CloseHandle(hr);
}

/* What the drive reports, in full: for when no audio is found. */
static void cd_diag(HANDLE h, char letter)
{
    TOC_DATA toc;
    static FULL_TOC ft;
    int cnt = 0;
    printf("\n--- Disc report for drive %c: (send a photo of this to the author) ---\n", letter);
    if (read_toc(h, &toc)) {
        printf("TOC: first track %d, last track %d\n", toc.FirstTrack, toc.LastTrack);
        for (int i = 0; i <= toc.LastTrack - toc.FirstTrack + 1 && i < 100; i++)
            printf("  entry %2d: number %3d, control/adr 0x%02X (%s), start %ld\n", i, toc.Tracks[i].TrackNumber,
                   toc.Tracks[i].Control_Adr, (toc.Tracks[i].Control_Adr & 4) ? "data" : "audio",
                   msf_to_lba(toc.Tracks[i].Address));
    } else {
        printf("TOC: cannot be read (error %lu)\n", (unsigned long)GetLastError());
    }
    if (read_toc(h, &toc) && toc.LastTrack == toc.FirstTrack && (toc.Tracks[0].Control_Adr & 4))
        probe_hidden(letter, msf_to_lba(toc.Tracks[1].Address));
    if (read_full_toc(h, &ft, &cnt)) {
        printf("Full TOC: sessions %d..%d, %d entries\n", ft.FirstSession, ft.LastSession, cnt);
        for (int i = 0; i < cnt; i++)
            printf("  session %d: point 0x%02X, control/adr 0x%02X, position %ld\n", ft.D[i].Session,
                   ft.D[i].Point, ft.D[i].Control_Adr, msf3_to_lba(ft.D[i].Msf));
    } else {
        printf("Full TOC: not available (error %lu)\n", (unsigned long)GetLastError());
    }
    /* the way the 1998 game itself asks: Windows' MCI "cdaudio" */
    {
        char ret[128];
        char cmd[96];
        MCIERROR e;
        snprintf(cmd, sizeof(cmd), "open %c: type cdaudio alias srdiag shareable", letter);
        e = mciSendStringA(cmd, ret, sizeof(ret), NULL);
        if (e) {
            mciGetErrorStringA(e, ret, sizeof(ret));
            printf("MCI cdaudio: cannot open (%s)\n", ret);
        } else {
            int n = 0;
            ret[0] = 0;
            mciSendStringA("status srdiag number of tracks", ret, sizeof(ret), NULL);
            n = atoi(ret);
            printf("MCI cdaudio: %d tracks\n", n);
            for (int t = 1; t <= n && t <= 99; t++) {
                char ty[64] = "?", ln[64] = "?";
                snprintf(cmd, sizeof(cmd), "status srdiag type track %d", t);
                mciSendStringA(cmd, ty, sizeof(ty), NULL);
                snprintf(cmd, sizeof(cmd), "status srdiag length track %d", t);
                mciSendStringA(cmd, ln, sizeof(ln), NULL);
                printf("  track %2d: %s, length %s\n", t, ty, ln);
            }
            mciSendStringA("close srdiag", NULL, 0, NULL);
        }
    }
    printf("--- end of report ---\n");
}

static int rip_cd(char letter, const char *outDir)
{
    HANDLE h = open_drive(letter);
    if (h == INVALID_HANDLE_VALUE) { printf("Cannot open drive %c: (error %lu). Try running as administrator.\n", letter, (unsigned long)GetLastError()); return 0; }
    TOC_DATA toc;
    if (!read_toc(h, &toc)) { printf("Drive %c: has no readable disc.\n", letter); cd_diag(h, letter); CloseHandle(h); return 0; }
    AUDIO_TRACK list[100];
    int fromFull = 0;
    int nTracks = list_audio_tracks(h, list, &fromFull);
    printf("Drive %c: %d tracks on the disc, %d audio%s.\n", letter, toc.LastTrack - toc.FirstTrack + 1, nTracks,
           fromFull ? " (found in another session)" : "");
    if (nTracks == 0) {
        printf("\nThis disc has no audio tracks that Windows can see, so there is no music to read.\n"
               "(The 1998 game plays its music from CD audio tracks only.)\n");
        cd_diag(h, letter);
        CloseHandle(h);
        return 0;
    }
    static BYTE buf[SECT_RAW * READ_SECTS];
    int done = 0;
    for (int i = 0; i < nTracks; i++) {
        long start = list[i].start, end = list[i].end;
        char name[32], out[1024];
        snprintf(name, sizeof(name), "track%d.son", list[i].number);
        join(out, sizeof(out), outDir, name);
        FILE *o = fopen(out, "wb");
        if (!o) { printf("Cannot write %s\n", out); continue; }
        long total = end - start, lastPct = -1, errors = 0;
        for (long s = start; s < end; ) {
            ULONG n = (ULONG)(end - s < READ_SECTS ? end - s : READ_SECTS);
            RAW_INFO ri;
            ri.DiskOffset.QuadPart = (LONGLONG)s * 2048;
            ri.SectorCount = n;
            ri.TrackMode = TRACKMODE_CDDA;
            DWORD got = 0;
            int ok = 0;
            for (int tr = 0; tr < 4 && !ok; tr++)
                ok = DeviceIoControl(h, IOCTL_RAW_READ, &ri, sizeof(ri), buf, SECT_RAW * n, &got, NULL) &&
                     got == SECT_RAW * n;
            if (!ok) {   /* unreadable spot: write silence and carry on */
                memset(buf, 0, SECT_RAW * n);
                errors++;
            }
            fwrite(buf, 1, SECT_RAW * n, o);
            s += n;
            long pct = (s - start) * 100 / total;
            if (pct != lastPct) {
                printf("\r  track %2d: %3ld%%", list[i].number, pct);
                fflush(stdout);
                lastPct = pct;
            }
        }
        fclose(o);
        printf("\r  track %2d -> %s (%ld:%02ld)%s\n", list[i].number, out,
               total / 75 / 60, (total / 75) % 60,
               errors ? "  [some read errors, filled with silence]" : "");
        if (errors == (total + READ_SECTS - 1) / READ_SECTS)
            printf("  WARNING: this drive cannot read audio from the disc, the track is silent.\n");
        done++;
    }
    CloseHandle(h);
    return done;
}

static char find_audio_cd(void)
{
    DWORD drives = GetLogicalDrives();
    for (char c = 'A'; c <= 'Z'; c++) {
        if (!(drives & (1u << (c - 'A')))) continue;
        char root[4] = { c, ':', '\\', 0 };
        if (GetDriveTypeA(root) != DRIVE_CDROM) continue;
        HANDLE h = open_drive(c);
        if (h == INVALID_HANDLE_VALUE) continue;
        AUDIO_TRACK list[100];
        int fromFull;
        int ok = list_audio_tracks(h, list, &fromFull) > 0;
        CloseHandle(h);
        if (ok) return c;
    }
    return 0;
}
#endif

/* ------------------------------------------------------------------ */

#ifdef _WIN32
static void exe_dir(char *out, size_t n, const char *argv0)
{
#ifdef _WIN32
    GetModuleFileNameA(NULL, out, (DWORD)n);
#else
    snprintf(out, n, "%s", argv0);
#endif
    char *s = strrchr(out, SEP);
    if (s) *s = 0; else snprintf(out, n, ".");
}

#endif

static void parent_dir(char *out, size_t n, const char *path)
{
    snprintf(out, n, "%s", path);
    char *a = strrchr(out, '\\'), *b = strrchr(out, '/');
    char *s = a > b ? a : b;
    if (s) *s = 0; else snprintf(out, n, ".");
}


/* ---- .cue / .bin disc images ------------------------------------------------ */
#ifdef _WIN32
#define SEEK64(f, o) _fseeki64((f), (long long)(o), SEEK_SET)
#else
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#endif

typedef struct { int num; int audio; int secsz; char file[1024]; long long start; long long end; } CUE_TRACK;

static int msf_text(const char *p, long *out)
{
    int m, s, f;
    if (sscanf(p, "%d:%d:%d", &m, &s, &f) != 3) return 0;
    *out = (m * 60 + s) * 75 + f;
    return 1;
}

static int rip_cue(const char *cue, const char *outDir)
{
    FILE *fc = fopen(cue, "r");
    if (!fc) { printf("Cannot open %s\n", cue); return 0; }
    static CUE_TRACK tr[100];
    int n = 0;
    char line[2048], curFile[1024] = "", dir[1024];
    parent_dir(dir, sizeof(dir), cue);
    while (fgets(line, sizeof(line), fc)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE", 4)) {
            char *a = strchr(p, '"'), *b = a ? strrchr(p, '"') : NULL;
            if (a && b > a) { *b = 0; snprintf(curFile, sizeof(curFile), "%s", a + 1); }
        } else if (!strncasecmp(p, "TRACK", 5) && n < 99) {
            int num = 0; char mode[32] = "";
            sscanf(p + 5, "%d %31s", &num, mode);
            CUE_TRACK *t = &tr[n++];
            memset(t, 0, sizeof(*t));
            t->num = num;
            t->audio = !strncasecmp(mode, "AUDIO", 5);
            t->secsz = 2352;
            if (!strncasecmp(mode, "MODE1/2048", 10)) t->secsz = 2048;
            snprintf(t->file, sizeof(t->file), "%s", curFile);
            t->start = -1;
        } else if (!strncasecmp(p, "INDEX", 5) && n > 0) {
            int idx = 0; char tm[32] = ""; long lba;
            sscanf(p + 5, "%d %31s", &idx, tm);
            if (idx == 1 && msf_text(tm, &lba)) tr[n - 1].start = (long long)lba * tr[n - 1].secsz;
        }
    }
    fclose(fc);
    int nAudio = 0;
    for (int i = 0; i < n; i++) {
        if (tr[i].audio) nAudio++;
        /* end = start of the next track in the same file, else the end of the file */
        char path[2048];
        join(path, sizeof(path), dir, tr[i].file);
        FILE *f = fopen(path, "rb");
        long long size = 0;
        if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        tr[i].end = size;
        if (i + 1 < n && !strcmp(tr[i + 1].file, tr[i].file) && tr[i + 1].start >= 0) tr[i].end = tr[i + 1].start;
    }
    printf("Disc image: %d track(s), %d audio.\n", n, nAudio);
    for (int i = 0; i < n; i++)
        printf("  track %2d  %s  %s  %lld MB\n", tr[i].num, tr[i].audio ? "audio" : "data ", tr[i].file,
               (tr[i].end - tr[i].start) / (1024 * 1024));
    if (!nAudio) {
        printf("\nThis image has no audio tracks, so there is no music in it that this tool can\n"
               "extract. Use WAV files of the soundtrack instead (track2.wav ... track21.wav).\n");
        return 0;
    }
    int done = 0;
    for (int i = 0; i < n; i++) {
        if (!tr[i].audio || tr[i].start < 0) continue;
        char path[2048], outp[2048], nm[32];
        join(path, sizeof(path), dir, tr[i].file);
        snprintf(nm, sizeof(nm), "track%d.son", tr[i].num);
        join(outp, sizeof(outp), outDir, nm);
        FILE *f = fopen(path, "rb"), *o = fopen(outp, "wb");
        if (!f || !o) { printf("  %s: cannot open files\n", nm); if (f) fclose(f); if (o) fclose(o); continue; }
        long long pos = tr[i].start, len = tr[i].end - tr[i].start;
        len -= len % 2352;
        SEEK64(f, pos);
        static unsigned char buf[2352 * 64];
        long long left = len;
        while (left > 0) {
            size_t want = left > (long long)sizeof(buf) ? sizeof(buf) : (size_t)left;
            size_t got = fread(buf, 1, want, f);
            if (!got) break;
            fwrite(buf, 1, got, o);
            left -= (long long)got;
        }
        fclose(f); fclose(o);
        printf("  %s: %lld MB\n", nm, (len - left) / (1024 * 1024));
        done++;
    }
    return done;
}

static int has_ext(const char *s, const char *e)
{
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcasecmp(s + a - b, e);
}

int main(int argc, char **argv)
{
    char outDir[1024] = "";
    const char *inputs[256];
    int nIn = 0;

    printf("Sonic R music converter for the Xbox port\n"
           "Output: MUSIC\\trackN.son (44100 Hz, 16-bit stereo PCM)\n\n");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) snprintf(outDir, sizeof(outDir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("sonicr_music              rip the original CD (first drive with audio)\n"
                   "sonicr_music E:           rip from drive E:\n"
                   "sonicr_music a.wav b.wav  convert WAV files (track number from the name)\n"
                   "sonicr_music folder       convert every .wav in the folder\n"
                   "sonicr_music disc.cue     extract the audio tracks of a .bin/.cue image\n"
                   "  -o DIR                  output folder (default: MUSIC)\n");
            return 0;
        }
        else if (nIn < 256) inputs[nIn++] = argv[i];
    }
#ifdef _WIN32
    /* Started from Explorer (double click or drag and drop): keep the window. */
    {
        DWORD pids[2];
        g_pause = GetConsoleProcessList(pids, 2) <= 1;
    }
#endif

    int done = 0;
    int isCd = (nIn == 0) || (nIn == 1 && strlen(inputs[0]) <= 3 && inputs[0][1] == ':' &&
                              (inputs[0][2] == 0 || inputs[0][2] == '\\'));
    if (isCd) {
#ifdef _WIN32
        char letter = nIn ? (char)toupper((unsigned char)inputs[0][0]) : find_audio_cd();
        if (!letter) {
            printf("No CD with audio tracks found. Put the Sonic R disc in the drive,\n"
                   "or drag your ripped WAV files onto this program.\n");
            {   /* what each CD drive reports, to see why */
                DWORD dv = GetLogicalDrives();
                for (char c = 'A'; c <= 'Z'; c++) {
                    char root[4] = { c, ':', '\\', 0 };
                    if (!(dv & (1u << (c - 'A'))) || GetDriveTypeA(root) != DRIVE_CDROM) continue;
                    HANDLE hh = open_drive(c);
                    if (hh == INVALID_HANDLE_VALUE) { printf("\nCannot open drive %c: (error %lu)\n", c, (unsigned long)GetLastError()); continue; }
                    cd_diag(hh, c);
                    CloseHandle(hh);
                }
            }
            finish(1);
        }
        if (!outDir[0]) {
            char d[1024];
            exe_dir(d, sizeof(d), argv[0]);
            join(outDir, sizeof(outDir), d, "MUSIC");
        }
        MKDIR(outDir);
        printf("Ripping the CD in drive %c: to %s\n", letter, outDir);
        done = rip_cd(letter, outDir);
#else
        printf("Reading CDs is only supported on Windows here. Rip the CD to WAV\n"
               "files with any ripper and pass them (or their folder) to this tool.\n");
        return 1;
#endif
    } else {
        if (!outDir[0]) {
            char d[1024];
            if (is_dir(inputs[0])) snprintf(d, sizeof(d), "%s", inputs[0]);
            else parent_dir(d, sizeof(d), inputs[0]);
            join(outDir, sizeof(outDir), d, "MUSIC");
        }
        MKDIR(outDir);
        printf("Converting to %s\n", outDir);
        for (int i = 0; i < nIn; i++) {
            if (has_ext(inputs[i], ".cue")) done += rip_cue(inputs[i], outDir);
            else if (has_ext(inputs[i], ".bin")) {
                char c[1024]; snprintf(c, sizeof(c), "%s", inputs[i]);
                strcpy(c + strlen(c) - 3, "cue");
                FILE *t = fopen(c, "r");
                if (t) { fclose(t); done += rip_cue(c, outDir); }
                else printf("  %s: give the .cue file of this image instead\n", inputs[i]);
            }
            else if (is_dir(inputs[i])) done += convert_folder(inputs[i], outDir);
            else if (has_wav_ext(inputs[i])) done += convert_wav(inputs[i], outDir);
            else printf("  %s: not a WAV file - skipped (convert it to WAV first)\n", inputs[i]);
        }
    }

    printf("\n%d track(s) written. Copy the MUSIC folder next to default.xbe on the Xbox.\n", done);
    finish(done > 0 ? 0 : 1);
    return 0;
}
