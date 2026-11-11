/* CD sources for the installer: an ISO 9660 image (.iso, raw .bin, .cue with
 * its tracks, Nero .nrg), a CD drive or a folder holding a copy of the disc.
 * Images also give the disc's audio tracks (cd_tracks / cd_read_raw) for
 * games with CD music. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "setup.h"

typedef struct { char name[64]; u32 lba, size; int dir; } IsoEnt;

typedef struct {
    CdTrack t;
    char file[MAX_PATH * 2];      /* image file holding the track */
    u64 off;                      /* file offset of its first sector (INDEX 01) */
    u32 sector;                   /* bytes per sector in the file */
} Trk;

struct CdSrc {
    int image;
    char path[MAX_PATH * 2];      /* image file or root folder */
    HANDLE h;
    u32 sector, data_off;         /* bytes per sector, user data offset in it */
    u64 base;                     /* file offset of the data track's first sector */
    u32 root_lba, root_size;
    char desc[MAX_PATH * 2 + 32];
    Trk tr[100];
    int ntr;
};

/* ---------- image access ---------- */
static int read_sectors(CdSrc *c, u32 lba, u32 count, u8 *out)
{
    for (u32 i = 0; i < count; i++) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)c->base + (LONGLONG)(lba + i) * c->sector + c->data_off;
        DWORD r = 0;
        if (!SetFilePointerEx(c->h, pos, NULL, FILE_BEGIN) || !ReadFile(c->h, out + i * 2048, 2048, &r, NULL) || r != 2048)
            return 0;
    }
    return 1;
}

static int probe(CdSrc *c, u32 sector, u32 off)
{
    u8 pvd[2048];
    c->sector = sector;
    c->data_off = off;
    if (!read_sectors(c, 16, 1, pvd)) return 0;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return 0;
    const u8 *root = pvd + 156;
    c->root_lba = root[2] | root[3] << 8 | root[4] << 16 | (u32)root[5] << 24;
    c->root_size = root[10] | root[11] << 8 | root[12] << 16 | (u32)root[13] << 24;
    return 1;
}

/* list one directory of the image */
static IsoEnt *iso_dir(CdSrc *c, u32 lba, u32 size, int *count)
{
    u32 ns = (size + 2047) / 2048;
    u8 *buf = malloc(ns * 2048);
    int cap = 64, n = 0;
    IsoEnt *e = malloc(cap * sizeof *e);
    if (!read_sectors(c, lba, ns, buf)) { free(buf); *count = 0; return e; }
    u32 p = 0;
    while (p < size) {
        u8 len = buf[p];
        if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
        u8 nl = buf[p + 32];
        const char *nm = (const char *)buf + p + 33;
        if (!(nl == 1 && (nm[0] == 0 || nm[0] == 1))) {
            if (n == cap) { cap *= 2; e = realloc(e, cap * sizeof *e); }
            IsoEnt *x = &e[n++];
            int k = 0;
            for (int i = 0; i < nl && nm[i] != ';' && k < 63; i++) x->name[k++] = nm[i];
            if (k && x->name[k - 1] == '.') k--;
            x->name[k] = 0;
            const u8 *r = buf + p;
            x->lba = r[2] | r[3] << 8 | r[4] << 16 | (u32)r[5] << 24;
            x->size = r[10] | r[11] << 8 | r[12] << 16 | (u32)r[13] << 24;
            x->dir = (r[25] & 2) != 0;
        }
        p += len;
    }
    free(buf);
    *count = n;
    return e;
}

static int iso_find(CdSrc *c, const char *rel, IsoEnt *out)
{
    u32 lba = c->root_lba, size = c->root_size;
    char part[64];
    const char *s = rel;
    while (*s) {
        int k = 0;
        while (*s && *s != '\\' && *s != '/' && k < 63) part[k++] = (char)toupper((u8)*s++);
        part[k] = 0;
        while (*s == '\\' || *s == '/') s++;
        int n;
        IsoEnt *e = iso_dir(c, lba, size, &n);
        int found = -1;
        for (int i = 0; i < n; i++) if (!_stricmp(e[i].name, part)) { found = i; break; }
        if (found < 0) { free(e); return 0; }
        *out = e[found];
        lba = e[found].lba;
        size = e[found].size;
        free(e);
    }
    return 1;
}

/* ---------- open ---------- */
static int open_image(CdSrc *c, const char *file)
{
    c->h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (c->h == INVALID_HANDLE_VALUE) return 0;
    if (probe(c, 2048, 0) || probe(c, 2352, 16) || probe(c, 2352, 24)) return 1;
    CloseHandle(c->h);
    c->h = INVALID_HANDLE_VALUE;
    return 0;
}


static u64 file_size(const char *f)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(f, GetFileExInfoStandard, &a)) return 0;
    return ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static u64 be64(const u8 *p) { u64 v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static u32 be32c(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Nero image: chunk list at the end (NER5 + u64 offset / NERO + u32), DAO
   track table (DAOX 64-bit / DAOI 32-bit offsets): sector size, mode,
   index 0, index 1, end of every track */
static int open_nrg(CdSrc *c, const char *file)
{
    HANDLE h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    u64 n = file_size(file);
    u8 tail[12];
    LARGE_INTEGER pos;
    DWORD r;
    pos.QuadPart = (LONGLONG)n - 12;
    if (n < 12 || !SetFilePointerEx(h, pos, NULL, FILE_BEGIN) || !ReadFile(h, tail, 12, &r, NULL)) { CloseHandle(h); return 0; }
    u64 at;
    if (!memcmp(tail, "NER5", 4)) at = be64(tail + 4);
    else if (!memcmp(tail + 4, "NERO", 4)) at = be32c(tail + 8);
    else { CloseHandle(h); return 0; }
    if (at >= n) { CloseHandle(h); return 0; }
    size_t len = (size_t)(n - at);
    u8 *d = malloc(len);
    pos.QuadPart = (LONGLONG)at;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    ReadFile(h, d, (DWORD)len, &r, NULL);
    CloseHandle(h);
    size_t p = 0;
    u32 cur = 0;
    while (p + 8 <= len) {
        u32 cl = be32c(d + p + 4);
        const u8 *q = d + p + 8;
        int x = !memcmp(d + p, "DAOX", 4);
        if (x || !memcmp(d + p, "DAOI", 4)) {
            size_t es = x ? 42 : 30;
            int first = q[20];
            for (size_t e = 22; e + es <= cl && c->ntr < 99; e += es) {
                const u8 *t = q + e;
                u32 ss = (u32)(t[12] << 8 | t[13]), mode = t[14];
                u64 i0 = x ? be64(t + 18) : be32c(t + 18), i1 = x ? be64(t + 26) : be32c(t + 22);
                u64 end = x ? be64(t + 34) : be32c(t + 26);
                if (!ss) continue;
                Trk *k = &c->tr[c->ntr];
                k->t.no = first + c->ntr;
                k->t.audio = mode == 7;            /* 7 = audio, 0 = mode 1 */
                cur += (u32)((i1 - i0) / ss);     /* pregap */
                k->t.lba = cur - 150;
                k->t.nsec = (u32)((end - i1) / ss);
                cur += k->t.nsec;
                k->off = i1;
                k->sector = ss;
                snprintf(k->file, sizeof k->file, "%s", file);
                c->ntr++;
            }
        }
        if (!memcmp(d + p, "END!", 4)) break;
        p += 8 + cl;
    }
    free(d);
    if (!c->ntr || c->tr[0].t.audio) return 0;
    c->h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    c->base = c->tr[0].off;
    u32 ss = c->tr[0].sector;
    if (probe(c, ss, 0) || (ss == 2352 && (probe(c, 2352, 16) || probe(c, 2352, 24)))) return 1;
    CloseHandle(c->h);
    c->h = INVALID_HANDLE_VALUE;
    return 0;
}

/* .cue: FILE / TRACK nn AUDIO|MODEx/size / INDEX 01 mm:ss:ff - one file for
   the whole disc or one per track */
static void parse_cue(CdSrc *c, const char *cuepath, char *cue)
{
    char dir[MAX_PATH * 2], file[MAX_PATH * 2] = "";
    snprintf(dir, sizeof dir, "%s", cuepath);
    char *bs = strrchr(dir, '\\');
    if (bs) bs[1] = 0; else dir[0] = 0;
    u32 file_lba = 0, ss = 2352;
    int cur = -1;
    for (char *line = strtok(cue, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "FILE", 4)) {
            if (file[0]) file_lba += (u32)(file_size(file) / ss);
            char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q2) continue;
            *q2 = 0;
            snprintf(file, sizeof file, "%s%s", strchr(q1 + 1, ':') ? "" : dir, q1 + 1);
        } else if (!strncmp(line, "TRACK", 5) && c->ntr < 99) {
            Trk *k = &c->tr[cur = c->ntr++];
            memset(k, 0, sizeof *k);
            k->t.no = atoi(line + 5);
            k->t.audio = strstr(line, "AUDIO") != NULL;
            ss = strstr(line, "/2048") ? 2048 : strstr(line, "/2336") ? 2336 : 2352;
            k->sector = ss;
            snprintf(k->file, sizeof k->file, "%s", file);
        } else if (!strncmp(line, "INDEX 01", 8) && cur >= 0) {
            int m = 0, s2 = 0, f = 0;
            sscanf(line + 8, "%d:%d:%d", &m, &s2, &f);
            u32 fr = (u32)((m * 60 + s2) * 75 + f);
            c->tr[cur].off = (u64)fr * ss;
            c->tr[cur].t.lba = file_lba + fr;
        }
    }
    for (int i = 0; i < c->ntr; i++) {
        Trk *k = &c->tr[i];
        u64 end = (i + 1 < c->ntr && !_stricmp(c->tr[i + 1].file, k->file)) ? c->tr[i + 1].off : file_size(k->file);
        k->t.nsec = end > k->off ? (u32)((end - k->off) / k->sector) : 0;
    }
}

int cd_tracks(CdSrc *c, CdTrack *out, int max)
{
    int n = 0;
    for (int i = 0; i < c->ntr && n < max; i++) out[n++] = c->tr[i].t;
    return n;
}

/* raw 2352-byte sectors of track i (audio: 16-bit stereo PCM, 44.1 kHz) */
int cd_read_raw(CdSrc *c, int i, u32 sec, u32 count, u8 *out)
{
    if (i < 0 || i >= c->ntr || c->tr[i].sector != 2352) return 0;
    Trk *k = &c->tr[i];
    HANDLE h = CreateFileA(k->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)(k->off + (u64)sec * 2352);
    DWORD r = 0;
    int ok = SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && ReadFile(h, out, count * 2352, &r, NULL) && r == count * 2352;
    CloseHandle(h);
    return ok;
}

CdSrc *cd_open(const char *path)
{
    CdSrc *c = calloc(1, sizeof *c);
    c->h = INVALID_HANDLE_VALUE;
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s", path);
    size_t L = strlen(p);
    while (L > 3 && (p[L - 1] == '\\' || p[L - 1] == '/')) p[--L] = 0;
    if (L == 2 && p[1] == ':') { p[2] = '\\'; p[3] = 0; }
    if (is_dir(p)) {
        snprintf(c->path, sizeof c->path, "%s", p);
        snprintf(c->desc, sizeof c->desc, "folder %s", p);
        return c;
    }
    const char *ext = strrchr(p, '.');
    if (ext && !_stricmp(ext, ".nrg")) {
        if (!open_nrg(c, p)) { free(c); return NULL; }
        c->image = 1;
        snprintf(c->path, sizeof c->path, "%s", p);
        snprintf(c->desc, sizeof c->desc, "Nero image %s (%d tracks)", p, c->ntr);
        return c;
    }
    if (ext && !_stricmp(ext, ".cue")) {
        size_t cn;
        char *cue2 = (char *)read_file(p, &cn);
        if (cue2) { cue2[cn] = 0; parse_cue(c, p, cue2); free(cue2); }
        /* FILE "name.bin" BINARY */
        size_t n;
        char *cue = (char *)read_file(p, &n);
        if (cue) {
            cue[n] = 0;
            char *f = strstr(cue, "FILE");
            char *q1 = f ? strchr(f, '"') : NULL;
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q2) {
                *q2 = 0;
                char dir[MAX_PATH * 2];
                snprintf(dir, sizeof dir, "%s", p);
                char *bs = strrchr(dir, '\\');
                if (bs) bs[1] = 0; else dir[0] = 0;
                snprintf(p, sizeof p, "%s%s", strchr(q1 + 1, ':') ? "" : dir, q1 + 1);
            }
            free(cue);
        }
    }
    if (!open_image(c, p)) { free(c); return NULL; }
    c->image = 1;
    snprintf(c->path, sizeof c->path, "%s", p);
    snprintf(c->desc, sizeof c->desc, "image %s", p);
    return c;
}

void cd_close(CdSrc *c)
{
    if (!c) return;
    if (c->h != INVALID_HANDLE_VALUE) CloseHandle(c->h);
    free(c);
}

const char *cd_describe(CdSrc *c) { return c->desc; }

int cd_exists(CdSrc *c, const char *rel)
{
    if (c->image) { IsoEnt e; return iso_find(c, rel, &e); }
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\%s", c->path, rel);
    return path_exists(p);
}

u8 *cd_read(CdSrc *c, const char *rel, size_t *n)
{
    if (!c->image) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", c->path, rel);
        return read_file(p, n);
    }
    IsoEnt e;
    if (!iso_find(c, rel, &e) || e.dir) return NULL;
    u32 ns = (e.size + 2047) / 2048;
    u8 *d = malloc(ns * 2048 + 1);
    if (!read_sectors(c, e.lba, ns, d)) { free(d); return NULL; }
    *n = e.size;
    return d;
}

/* ---------- walking ---------- */
static u64 iso_total(CdSrc *c, u32 lba, u32 size, int depth)
{
    int n;
    IsoEnt *e = iso_dir(c, lba, size, &n);
    u64 t = 0;
    for (int i = 0; i < n; i++)
        t += e[i].dir ? (depth < 16 ? iso_total(c, e[i].lba, e[i].size, depth + 1) : 0) : e[i].size;
    free(e);
    return t;
}

static u64 dir_total(const char *dir, int depth)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    u64 t = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (depth < 16) t += dir_total(p, depth + 1);
        } else t += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return t;
}

u64 cd_total_bytes(CdSrc *c)
{
    return c->image ? iso_total(c, c->root_lba, c->root_size, 0) : dir_total(c->path, 0);
}

static int iso_copy(CdSrc *c, u32 lba, u32 size, const char *dst, Progress *pr, int depth)
{
    int n;
    IsoEnt *e = iso_dir(c, lba, size, &n);
    make_dirs(dst);
    static u8 buf[32 * 2048];
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", dst, e[i].name);
        if (e[i].dir) { if (depth < 16) ok = iso_copy(c, e[i].lba, e[i].size, p, pr, depth + 1); continue; }
        HANDLE out = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { setup_log("cannot write %s", p); ok = 0; break; }
        u32 left = e[i].size, l = e[i].lba;
        while (left && ok) {
            u32 ns = (left + 2047) / 2048;
            if (ns > 32) ns = 32;
            if (!read_sectors(c, l, ns, buf)) { setup_log("read error in %s", e[i].name); ok = 0; break; }
            u32 chunk = ns * 2048 < left ? ns * 2048 : left;
            DWORD w;
            if (!WriteFile(out, buf, chunk, &w, NULL) || w != chunk) { ok = 0; break; }
            left -= chunk;
            l += ns;
            if (pr) progress_add(pr, chunk);
        }
        CloseHandle(out);
    }
    free(e);
    return ok;
}

static int dir_copy(const char *src, const char *dst, Progress *pr, int depth)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    make_dirs(dst);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char s[MAX_PATH * 2], d[MAX_PATH * 2];
        snprintf(s, sizeof s, "%s\\%s", src, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 16) ok = dir_copy(s, d, pr, depth + 1); }
        else {
            ok = copy_file(s, d, pr);
            SetFileAttributesA(d, FILE_ATTRIBUTE_NORMAL);   /* CD files come read-only */
        }
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

int cd_copy_all(CdSrc *c, const char *dst, Progress *pr)
{
    return c->image ? iso_copy(c, c->root_lba, c->root_size, dst, pr, 0) : dir_copy(c->path, dst, pr, 0);
}
