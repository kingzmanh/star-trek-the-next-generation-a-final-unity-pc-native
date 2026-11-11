/* ZIP reader for the installer: stored and deflated entries (RFC 1951),
 * read straight from the archive file (the game's STTNG.ZIP is 476 MB).
 * zip_extract() writes every wanted entry (name without its path) into a
 * folder. */
#include "setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unzip.h"

/* ---------- inflate ---------- */
typedef struct {
    const u8 *in; size_t inlen, inpos;
    u32 bitbuf; int bitcnt;
    u8 *out; size_t outlen, outpos;
    int err;
} Inf;

static int bits(Inf *s, int need)
{
    u32 v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (u32)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

typedef struct { short count[16], symbol[288]; } Huff;

static int decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1;
    return -1;
}

static int construct(Huff *h, const short *length, int n)
{
    short offs[16];
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(Inf *s, const Huff *lencode, const Huff *distcode)
{
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -1;
            s->out[s->outpos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lbase[sym] + bits(s, lext[sym]);
            int ds = decode(s, distcode);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)dbase[ds] + (size_t)bits(s, dext[ds]);
            if (dist > s->outpos || s->outpos + (size_t)len > s->outlen) return -1;
            for (int i = 0; i < len; i++) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
        }
        if (s->err) return -1;
    }
}

static int fixed(Inf *s)
{
    static Huff lc, dc;
    static int done;
    if (!done) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&dc, l, 30);
        done = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(Inf *s)
{
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    Huff lc, dc;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    int i;
    for (i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(s, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (construct(&lc, lengths, 19) != 0) return -1;
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return -1; len = lengths[idx - 1]; rep = 3 + bits(s, 2); }
            else if (sym == 17) rep = 3 + bits(s, 3);
            else rep = 11 + bits(s, 7);
            if (idx + rep > nlen + ndist) return -1;
            while (rep--) lengths[idx++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = construct(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return codes(s, &lc, &dc);
}

static int inflate_buf(const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
    Inf s = {0};
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outlen;
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        int r;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) return -1;
            u32 len = s.in[s.inpos] | s.in[s.inpos + 1] << 8;
            s.inpos += 4;
            if (s.inpos + len > s.inlen || s.outpos + len > s.outlen) return -1;
            memcpy(s.out + s.outpos, s.in + s.inpos, len);
            s.inpos += len; s.outpos += len;
            r = 0;
        } else if (type == 1) r = fixed(&s);
        else if (type == 2) r = dynamic(&s);
        else r = -1;
        if (r || s.err) return -1;
    } while (!last);
    return s.outpos == outlen ? 0 : -1;
}

/* ---------- zip ---------- */
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

int zip_extract(const char *zip, const char *dstdir, int (*want)(const char *name, void *ud), void *ud,
                void (*progress)(u64 bytes, void *ud))
{
    FILE *f = fopen(zip, "rb");
    if (!f) { setup_log("unzip: cannot open %s", zip); return -1; }
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    /* end of central directory record: within the last 64 KB + 22 */
    long long tail = size < 65557 ? size : 65557;
    u8 *t = malloc((size_t)tail);
    _fseeki64(f, size - tail, SEEK_SET);
    if (fread(t, 1, (size_t)tail, f) != (size_t)tail) { free(t); fclose(f); return -1; }
    long long eocd = -1;
    for (long long i = tail - 22; i >= 0; i--) if (le32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); fclose(f); setup_log("unzip: %s is not a zip", zip); return -1; }
    u32 nent = le16(t + eocd + 10), cdsize = le32(t + eocd + 12), cdoff = le32(t + eocd + 16);
    free(t);
    u8 *cdir = malloc(cdsize);
    _fseeki64(f, cdoff, SEEK_SET);
    if (fread(cdir, 1, cdsize, f) != cdsize) { free(cdir); fclose(f); return -1; }
    int count = 0;
    u32 p = 0;
    for (u32 e = 0; e < nent && p + 46 <= cdsize; e++) {
        const u8 *h = cdir + p;
        if (le32(h) != 0x02014b50) break;
        u16 method = le16(h + 10), nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
        u32 csize = le32(h + 20), usize = le32(h + 24), loff = le32(h + 42);
        char name[260];
        int n = nlen < 259 ? nlen : 259;
        memcpy(name, h + 46, n);
        name[n] = 0;
        p += 46 + nlen + xlen + clen;
        const char *base = name;
        for (const char *q = name; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
        if (!*base || !want(base, ud)) continue;
        u8 lh[30];
        _fseeki64(f, loff, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50) { setup_log("unzip: bad entry %s", name); continue; }
        _fseeki64(f, (long long)loff + 30 + le16(lh + 26) + le16(lh + 28), SEEK_SET);
        u8 *cbuf = malloc(csize + 1), *ubuf = malloc(usize + 1);
        int ok = fread(cbuf, 1, csize, f) == csize;
        if (ok && method == 0 && csize == usize) memcpy(ubuf, cbuf, usize);
        else if (ok && method == 8) ok = inflate_buf(cbuf, csize, ubuf, usize) == 0;
        else ok = 0;
        if (ok) {
            char out[MAX_PATH * 2];
            snprintf(out, sizeof out, "%s\\%s", dstdir, base);
            FILE *o = fopen(out, "wb");
            ok = o && fwrite(ubuf, 1, usize, o) == usize;
            if (o) fclose(o);
            if (ok) count++;
        }
        if (!ok) setup_log("unzip: could not extract %s (method %u)", name, method);
        if (progress) progress(csize, ud);
        free(cbuf);
        free(ubuf);
    }
    free(cdir);
    fclose(f);
    return count;
}
