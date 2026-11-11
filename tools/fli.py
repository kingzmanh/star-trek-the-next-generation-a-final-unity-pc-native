"""Autodesk FLI/FLC reader (Quarantine's TITLE.FLI, END.FLI, INTRO.FLI):
decodes frames to 8-bit images + palette.  usage: fli.py <file.fli> <frame> <out.png>"""
import struct, sys


def frames(path):
    d = open(path, 'rb').read()
    size, magic, nframes, w, h = struct.unpack_from('<IHHHH', d, 0)
    pix = bytearray(w * h)
    pal = [0] * 768
    six = magic == 0xAF11          # FLI: 6-bit palette
    p = 128
    for _ in range(nframes):
        fsize, ftype, nchunks = struct.unpack_from('<IHH', d, p)
        q = p + 16
        for _ in range(nchunks):
            csize, ctype = struct.unpack_from('<IH', d, q)
            c = q + 6
            if ctype in (4, 11):   # COLOR_256 / COLOR_64
                n = struct.unpack_from('<H', d, c)[0]; c += 2; idx = 0
                for _ in range(n):
                    idx += d[c]; cnt = d[c + 1] or 256; c += 2
                    for i in range(cnt):
                        for k in range(3):
                            v = d[c]; c += 1
                            pal[(idx + i) * 3 + k] = (v << 2 | v >> 4) if ctype == 11 else v
                    idx += cnt
            elif ctype == 13:      # BLACK
                pix[:] = bytes(w * h)
            elif ctype == 16:      # COPY
                pix[:] = d[c:c + w * h]
            elif ctype == 15:      # BRUN
                for y in range(h):
                    c += 1; x = 0
                    while x < w:
                        n = struct.unpack_from('b', d, c)[0]; c += 1
                        if n >= 0:
                            pix[y * w + x:y * w + x + n] = bytes([d[c]]) * n; c += 1; x += n
                        else:
                            n = -n; pix[y * w + x:y * w + x + n] = d[c:c + n]; c += n; x += n
            elif ctype == 12:      # LC (FLI delta)
                y0, nl = struct.unpack_from('<HH', d, c); c += 4
                for y in range(y0, y0 + nl):
                    npk = d[c]; c += 1; x = 0
                    for _ in range(npk):
                        x += d[c]; n = struct.unpack_from('b', d, c + 1)[0]; c += 2
                        if n >= 0:
                            pix[y * w + x:y * w + x + n] = d[c:c + n]; c += n; x += n
                        else:
                            n = -n; pix[y * w + x:y * w + x + n] = bytes([d[c]]) * n; c += 1; x += n
            elif ctype == 7:       # DELTA_FLC
                nl = struct.unpack_from('<H', d, c)[0]; c += 2; y = 0
                while nl > 0:
                    v = struct.unpack_from('<h', d, c)[0]; c += 2
                    if v < 0:
                        if v & 0x4000: y += -v
                        else: pix[y * w + w - 1] = v & 0xff
                        continue
                    x = 0
                    for _ in range(v):
                        x += d[c]; n = struct.unpack_from('b', d, c + 1)[0]; c += 2
                        if n >= 0:
                            pix[y * w + x:y * w + x + 2 * n] = d[c:c + 2 * n]; c += 2 * n; x += 2 * n
                        else:
                            n = -n; pix[y * w + x:y * w + x + 2 * n] = d[c:c + 2] * n; c += 2; x += 2 * n
                    y += 1; nl -= 1
            q += csize
        p += fsize
        yield w, h, bytes(pix), list(pal)


if __name__ == '__main__':
    from PIL import Image
    want = int(sys.argv[2])
    for i, (w, h, pix, pal) in enumerate(frames(sys.argv[1])):
        if i == want:
            im = Image.frombytes('P', (w, h), pix)
            im.putpalette(pal)
            im.convert('RGB').save(sys.argv[3])
            break
