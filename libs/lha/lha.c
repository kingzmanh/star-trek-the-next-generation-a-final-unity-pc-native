/* LHA archive reader for the installer: header levels 0/1/2, methods
 * -lh0- (stored) and -lh5- (LZ + static Huffman, 8 KB window).
 * Used for the 1.03 update (an LHA self-extractor) and CD 1's INSTALL.CR. */
#include <stdlib.h>
#include <string.h>
#include "setup.h"

/* ---------------- -lh5- decoder ---------------- */
#define DICBIT 13
#define DICSIZ (1u << DICBIT)
#define NC 510            /* 256 + 256 - 3 + 1 */
#define NP 14
#define NT 19
#define PBIT 4
#define TBIT 5
#define CBIT 9
#define NPT 0x80

typedef struct {
    const u8 *in;
    size_t inlen, inpos;
    unsigned bitbuf, subbitbuf, bitcount;
    unsigned blocksize;
    u16 left[2 * NC - 1], right[2 * NC - 1];
    u8 c_len[NC], pt_len[NPT];
    u16 c_table[4096], pt_table[256];
    int bad;
} Lh5;

static void fillbuf(Lh5 *s, unsigned n)
{
    s->bitbuf = (s->bitbuf << n) & 0xffff;
    while (n > s->bitcount) {
        n -= s->bitcount;
        s->bitbuf |= (s->subbitbuf << n) & 0xffff;
        s->subbitbuf = s->inpos < s->inlen ? s->in[s->inpos++] : 0;
        s->bitcount = 8;
    }
    s->bitcount -= n;
    s->bitbuf = (s->bitbuf | (s->subbitbuf >> s->bitcount)) & 0xffff;
}

static unsigned getbits(Lh5 *s, unsigned n)
{
    unsigned x = s->bitbuf >> (16 - n);
    fillbuf(s, n);
    return x;
}

static void make_table(Lh5 *s, int nchar, const u8 *bitlen, int tablebits, u16 *table)
{
    unsigned count[17], weight[17], start[18];
    unsigned i, k, len, ch, jutbits, avail, nextcode, mask;
    u16 *p;
    for (i = 1; i <= 16; i++) count[i] = 0;
    for (i = 0; i < (unsigned)nchar; i++) if (bitlen[i] <= 16) count[bitlen[i]]++;
    start[1] = 0;
    for (i = 1; i <= 16; i++) start[i + 1] = start[i] + (count[i] << (16 - i));
    if (start[17] != (1u << 16)) { s->bad = 1; return; }
    jutbits = 16 - tablebits;
    for (i = 1; i <= (unsigned)tablebits; i++) { start[i] >>= jutbits; weight[i] = 1u << (tablebits - i); }
    while (i <= 16) { weight[i] = 1u << (16 - i); i++; }
    i = start[tablebits + 1] >> jutbits;
    if (i != (1u << tablebits)) { k = 1u << tablebits; while (i != k) table[i++] = 0; }
    avail = nchar;
    mask = 1u << (15 - tablebits);
    for (ch = 0; ch < (unsigned)nchar; ch++) {
        if ((len = bitlen[ch]) == 0) continue;
        nextcode = start[len] + weight[len];
        if (len <= (unsigned)tablebits) {
            for (i = start[len]; i < nextcode; i++) table[i] = (u16)ch;
        } else {
            k = start[len];
            p = &table[k >> jutbits];
            i = len - tablebits;
            while (i != 0) {
                if (*p == 0) {
                    if (avail >= 2 * NC - 1) { s->bad = 1; return; }
                    s->right[avail] = s->left[avail] = 0;
                    *p = (u16)avail++;
                }
                p = (k & mask) ? &s->right[*p] : &s->left[*p];
                k <<= 1;
                i--;
            }
            *p = (u16)ch;
        }
        start[len] = nextcode;
    }
}

static void read_pt_len(Lh5 *s, int nn, int nbit, int i_special)
{
    int i, c, n = getbits(s, nbit);
    if (n == 0) {
        c = getbits(s, nbit);
        for (i = 0; i < nn; i++) s->pt_len[i] = 0;
        for (i = 0; i < 256; i++) s->pt_table[i] = (u16)c;
        return;
    }
    i = 0;
    while (i < n && i < NPT) {
        c = s->bitbuf >> 13;
        if (c == 7) {
            unsigned mask = 1u << 12;
            while (mask & s->bitbuf) { mask >>= 1; c++; }
        }
        fillbuf(s, (c < 7) ? 3 : c - 3);
        s->pt_len[i++] = (u8)c;
        if (i == i_special) {
            c = getbits(s, 2);
            while (--c >= 0 && i < NPT) s->pt_len[i++] = 0;
        }
    }
    while (i < nn) s->pt_len[i++] = 0;
    make_table(s, nn, s->pt_len, 8, s->pt_table);
}

static void read_c_len(Lh5 *s)
{
    int i, c, n = getbits(s, CBIT);
    if (n == 0) {
        c = getbits(s, CBIT);
        for (i = 0; i < NC; i++) s->c_len[i] = 0;
        for (i = 0; i < 4096; i++) s->c_table[i] = (u16)c;
        return;
    }
    i = 0;
    while (i < n && i < NC) {
        c = s->pt_table[s->bitbuf >> 8];
        if (c >= NT) {
            unsigned mask = 1u << 7;
            do {
                c = (s->bitbuf & mask) ? s->right[c] : s->left[c];
                mask >>= 1;
            } while (c >= NT && mask);
        }
        fillbuf(s, s->pt_len[c]);
        if (c <= 2) {
            if (c == 0) c = 1;
            else if (c == 1) c = getbits(s, 4) + 3;
            else c = getbits(s, CBIT) + 20;
            while (--c >= 0 && i < NC) s->c_len[i++] = 0;
        } else s->c_len[i++] = (u8)(c - 2);
    }
    while (i < NC) s->c_len[i++] = 0;
    make_table(s, NC, s->c_len, 12, s->c_table);
}

static unsigned decode_c(Lh5 *s)
{
    unsigned j, mask;
    if (s->blocksize == 0) {
        s->blocksize = getbits(s, 16);
        read_pt_len(s, NT, TBIT, 3);
        read_c_len(s);
        read_pt_len(s, NP, PBIT, -1);
    }
    s->blocksize--;
    j = s->c_table[s->bitbuf >> 4];
    if (j >= NC) {
        mask = 1u << 3;
        do {
            j = (s->bitbuf & mask) ? s->right[j] : s->left[j];
            mask >>= 1;
        } while (j >= NC && mask);
    }
    fillbuf(s, s->c_len[j]);
    return j;
}

static unsigned decode_p(Lh5 *s)
{
    unsigned j = s->pt_table[s->bitbuf >> 8], mask;
    if (j >= NP) {
        mask = 1u << 7;
        do {
            j = (s->bitbuf & mask) ? s->right[j] : s->left[j];
            mask >>= 1;
        } while (j >= NP && mask);
    }
    fillbuf(s, s->pt_len[j]);
    if (j != 0) j = (1u << (j - 1)) + getbits(s, j - 1);
    return j;
}

static int lh5_decode(const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
    Lh5 *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->in = in;
    s->inlen = inlen;
    fillbuf(s, 16);
    size_t o = 0;
    while (o < outlen && !s->bad) {
        unsigned c = decode_c(s);
        if (c < 256) out[o++] = (u8)c;
        else {
            unsigned len = c - 253;
            unsigned pos = decode_p(s) + 1;
            if (pos > o) { s->bad = 1; break; }
            while (len-- && o < outlen) { out[o] = out[o - pos]; o++; }
        }
    }
    int ok = !s->bad && o == outlen;
    free(s);
    return ok;
}

/* ---------------- archive walking ---------------- */
static u16 crc16_tab[256];
static void crc16_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        unsigned r = i;
        for (int k = 0; k < 8; k++) r = (r & 1) ? (r >> 1) ^ 0xa001 : r >> 1;
        crc16_tab[i] = (u16)r;
    }
}
static u16 crc16(const u8 *p, size_t n)
{
    u16 c = 0;
    if (!crc16_tab[1]) crc16_init();
    while (n--) c = crc16_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* does a plausible level 0/1/2 header start at off? */
static int lha_header_at(const u8 *d, size_t n, size_t off)
{
    if (off + 24 > n) return 0;
    const u8 *h = d + off;
    if (h[2] != '-' || h[3] != 'l' || h[4] != 'h' || h[6] != '-') return 0;
    int level = h[20];
    if (level > 2) return 0;
    if (level < 2) {
        unsigned hs = h[0];
        if (hs < 22 || off + hs + 2 > n) return 0;
        u8 sum = 0;
        for (unsigned i = 0; i < hs; i++) sum += h[2 + i];
        return sum == h[1];
    }
    return 1;
}

/* extract every file of the LHA archive found in data[] (an SFX may have a
   stub in front) into dir; returns files extracted or -1 on error */
int lha_extract(const u8 *d, size_t n, const char *dir, Progress *pr)
{
    size_t off = 0;
    while (off < n && !lha_header_at(d, n, off)) off++;
    if (off >= n) { setup_log("LHA: no archive found"); return -1; }
    int files = 0;
    while (off + 22 <= n && d[off] != 0) {
        const u8 *h = d + off;
        char method[6];
        memcpy(method, h + 2, 5);
        method[5] = 0;
        u32 packed = rd32(h + 7), orig = rd32(h + 11);
        int level = h[20];
        char name[260];
        size_t data_off;
        u16 crc = 0;
        if (level < 2) {
            unsigned hs = h[0];
            unsigned nl = h[21];
            if (nl >= sizeof name) return -1;
            memcpy(name, h + 22, nl);
            name[nl] = 0;
            crc = rd16(h + 22 + nl);
            data_off = off + 2 + hs;
            if (level == 1) {
                /* extended headers are counted in the packed size */
                size_t e = data_off - 2;
                unsigned next = rd16(d + e);
                while (next) {
                    if (e + 2 + next > n) return -1;
                    const u8 *x = d + data_off;
                    if (x[0] == 1 && next - 3 < sizeof name) {       /* file name */
                        memcpy(name, x + 1, next - 3);
                        name[next - 3] = 0;
                    }
                    data_off += next;
                    packed -= next;
                    next = rd16(d + data_off - 2);
                }
            }
        } else {
            unsigned total = rd16(h);
            crc = rd16(h + 21);
            size_t e = off + 24;
            name[0] = 0;
            unsigned next = rd16(h + 24 - 2);
            e = off + 26;
            (void)e;
            /* level 2: ext headers start at off+24, each { u16 size, type, data } */
            size_t x = off + 24;
            next = rd16(d + x);
            while (next && x + next <= off + total) {
                const u8 *t = d + x + 2;
                if (t[0] == 1 && next - 3 < sizeof name) { memcpy(name, t + 1, next - 3); name[next - 3] = 0; }
                x += next;
                next = rd16(d + x);
            }
            data_off = off + total;
        }
        if (data_off + packed > n) { setup_log("LHA: truncated archive"); return -1; }
        for (char *c = name; *c; c++) if (*c == '/' || (u8)*c == 0xff) *c = '\\';
        u8 *out = malloc(orig ? orig : 1);
        int ok;
        if (!strcmp(method, "-lh0-")) { ok = packed == orig; if (ok) memcpy(out, d + data_off, orig); }
        else if (!strcmp(method, "-lh5-")) ok = lh5_decode(d + data_off, packed, out, orig);
        else { setup_log("LHA: unsupported method %s for %s", method, name); ok = 0; }
        if (ok && crc16(out, orig) != crc) { setup_log("LHA: CRC error in %s", name); ok = 0; }
        if (!ok) { free(out); return -1; }
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\%s", dir, name);
        if (!write_file(path, out, orig)) { free(out); return -1; }
        free(out);
        files++;
        if (pr) progress_add(pr, orig);
        off = data_off + packed;
    }
    return files;
}
