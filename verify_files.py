#!/usr/bin/env python3
#
# Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
#
# independent check of modified files (Python standard library only):
#   * the SHA1 of the file ends with at least the requested number of zero bits (default 32)
#   * the file is still a valid PDF / PNG / PPM / ZIP file
#
# usage: python3 verify_files.py [-z bits] file ...
# exit status: 0 if every file passes, 1 otherwise
#

import hashlib, re, struct, sys, zipfile, zlib


def trailing_zero_bits(digest):
    n = int.from_bytes(digest, "big")
    return 160 if n == 0 else (n & -n).bit_length() - 1


def check_pdf(d):
    if not d.startswith(b"%PDF-"):
        return "missing %PDF- header"
    if b"%%EOF" not in d[-64:]:
        return "missing %%EOF at the end"
    m = re.search(rb"startxref\s+(\d+)\s+%%EOF\s*$", d)
    if m is None:
        return "missing startxref"
    off = int(m.group(1))
    tail = d[off:off + 64]
    if not (tail.startswith(b"xref") or re.match(rb"\d+\s+\d+\s+obj", tail)):
        return "startxref does not point to a cross-reference section"
    return None


def check_png(d):
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return "bad PNG signature"
    pos, idat, seen_iend = 8, b"", False
    width = height = None
    while pos < len(d):
        if pos + 12 > len(d):
            return "truncated chunk"
        (length,) = struct.unpack(">I", d[pos:pos + 4])
        if pos + 12 + length > len(d):
            return "truncated chunk"
        ctype = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", d[pos + 8 + length:pos + 12 + length])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            return "bad CRC in chunk %s" % ctype.decode("latin-1")
        if ctype == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            seen_iend = True
            if pos + 12 + length != len(d):
                return "data after IEND"
        pos += 12 + length
    if width is None or not seen_iend:
        return "missing IHDR or IEND"
    try:
        zlib.decompress(idat)
    except zlib.error as e:
        return "image data does not decompress (%s)" % e
    return None


def check_ppm(d):
    m = re.match(rb"P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s", d)
    if m is None:
        return "bad P6 header"
    w, h, maxval = map(int, m.groups())
    if maxval != 255:
        return "maxval is not 255"
    if len(d) - m.end() != 3 * w * h:
        return "pixel data has %d bytes, expected %d" % (len(d) - m.end(), 3 * w * h)
    return None


def check_zip(name):
    try:
        with zipfile.ZipFile(name) as z:
            bad = z.testzip()
            if bad is not None:
                return "bad CRC in member %s" % bad
    except (zipfile.BadZipFile, zlib.error, EOFError, OSError) as e:
        return str(e)
    return None


def main():
    args, bits = sys.argv[1:], 32
    if len(args) >= 2 and args[0] == "-z":
        bits, args = int(args[1]), args[2:]
    if not args:
        print("usage: python3 verify_files.py [-z bits] file ...")
        return 1
    ok = True
    for name in args:
        d = open(name, "rb").read()
        digest = hashlib.sha1(d).digest()
        zeros = trailing_zero_bits(digest)
        if d.startswith(b"%PDF"):
            fmt, err = "PDF", check_pdf(d)
        elif d.startswith(b"\x89PNG"):
            fmt, err = "PNG", check_png(d)
        elif d.startswith(b"P6"):
            fmt, err = "PPM", check_ppm(d)
        elif d.startswith(b"PK"):
            fmt, err = "ZIP", check_zip(name)
        else:
            fmt, err = "?", "unknown format"
        if zeros < bits:
            err = (err + "; " if err else "") + "only %d trailing zero bits" % zeros
        print("%-4s %s  %3d zero bits  %s  %s" % ("OK" if err is None else "FAIL", digest.hex(), zeros, fmt, name)
              + ("" if err is None else "  <-- " + err))
        ok = ok and err is None
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
