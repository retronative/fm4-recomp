"""Compare an FM4_DUMP_TEXTURE dump (runtime/video/texture_dump.h) against an independent untile.

Reads <prefix>_info.txt, <prefix>_level<k>_guest.bin and <prefix>_staged.bin (prefix e.g. <captures>/texdump_117D8000_0).
For every level of a 2D texture it untiles the guest bytes with an independent transcription of Xenia xenia-canary
67f16c4e315f texture_util.cc:443-458 (GetTiledOffset2D), or the linear rule (row pitch bytes), from the level's origin
block, applies the fetch endian per block (xenos.h:1070-1088; endian.xesli:25-46: 8in16 swaps the bytes of each 16-bit
half, 8in32 reverses each dword, 16in32 swaps the halves; 2-byte blocks only 8in16; 1-byte blocks none), and compares
byte for byte with what the renderer staged. For 4-byte blocks it also reports the A byte (byte 3 of the staged texel,
R8G8B8A8's alpha = the fetch's W) over a rectangle of level 0, and can write it as a PGM image.

    /opt/homebrew/bin/python3 -I compare_texdump.py <prefix> [--rect X0 Y0 X1 Y1] [--alpha-pgm OUT]
    /opt/homebrew/bin/python3 -I compare_texdump.py --self-test <empty scratch dir>
"""
import argparse
import os
import sys


def align(v, a):
    return (v + a - 1) // a * a


def tiled_offset_2d(x, y, pitch, log2):
    pitch = align(pitch, 32)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)


def swap(block, endian):
    if endian == 0:
        return bytes(block)
    if len(block) == 1:
        raise ValueError("1-byte blocks take no endian")
    if len(block) == 2:
        return bytes(block[::-1]) if endian == 1 else bytes(block)
    order = {1: (1, 0, 3, 2), 2: (3, 2, 1, 0), 3: (2, 3, 0, 1)}[endian]
    return bytes(block[i + k] for i in range(0, len(block), 4) for k in order)


def field(tokens, name, count=1):
    i = tokens.index(name)
    values = tokens[i + 1:i + 1 + count]
    return values[0] if count == 1 else values


def parse_info(path):
    lines = [l.split() for l in open(path) if l.strip()]
    header = [t for l in lines if l[0] != "level" for t in l]
    info = {
        "words": field(header, "words", 6),
        "dimension": int(field(header, "dimension")),
        "tiled": field(header, "tiled") == "1",
        "endian": int(field(header, "endian")),
        "bpb": int(field(header, "bytes_per_block")),
        "levels": [],
    }
    for l in (l for l in lines if l[0] == "level"):
        w, h = field(l, "blocks", 2)
        info["levels"].append({
            "index": int(l[1]), "x": int(field(l, "x")), "y": int(field(l, "y")), "wb": int(w), "hb": int(h),
            "pitch_blocks": int(field(l, "pitch_blocks")), "row_pitch": int(field(l, "row_pitch_bytes")),
            "offset": int(field(l, "staged_offset")),
        })
    return info


def compare(prefix, rect=None, alpha_pgm=None):
    info = parse_info(prefix + "_info.txt")
    print("words", " ".join(info["words"]))
    if info["dimension"] != 1:
        print("only 2D levels are compared (dimension %d)" % info["dimension"])
        return 2
    bpb, endian = info["bpb"], info["endian"]
    log2 = bpb.bit_length() - 1
    staged = open(prefix + "_staged.bin", "rb").read()
    failures = 0
    for lv in info["levels"]:
        k = lv["index"]
        guest = open("%s_level%d_guest.bin" % (prefix, k), "rb").read()
        wb, hb, offset = lv["wb"], lv["hb"], lv["offset"]
        mismatches, first = 0, None
        for y in range(hb):
            for x in range(wb):
                gx, gy = lv["x"] + x, lv["y"] + y
                src = tiled_offset_2d(gx, gy, lv["pitch_blocks"], log2) if info["tiled"] else gy * lv["row_pitch"] + (gx << log2)
                want = swap(guest[src:src + bpb], endian)
                at = offset + (y * wb + x) * bpb
                if want != staged[at:at + bpb]:
                    mismatches += 1
                    first = first or (x, y, want.hex(), staged[at:at + bpb].hex())
        print("level %d: %dx%d blocks, %d mismatching block(s)%s" % (k, wb, hb, mismatches, "" if first is None else ", first %s" % (first,)))
        failures += mismatches
        if k == 0 and bpb == 4:
            if rect:
                x0, y0, x1, y1 = rect
                alphas = [staged[offset + (y * wb + x) * 4 + 3] for y in range(max(y0, 0), min(y1, hb)) for x in range(max(x0, 0), min(x1, wb))]
                if alphas:
                    nonzero = sum(1 for v in alphas if v)
                    faint = sum(1 for v in alphas if 0 < v <= 11)  # (v / 255)^2 * 255 < 0.5: 0 if the alpha test saw 8-bit alpha
                    print("level 0 A in x [%d, %d) y [%d, %d): min %d max %d; %d of %d non-zero, %d of them <= 11"
                          % (x0, x1, y0, y1, min(alphas), max(alphas), nonzero, len(alphas), faint))
            if alpha_pgm:
                with open(alpha_pgm, "wb") as f:
                    f.write(b"P5 %d %d 255\n" % (wb, hb))
                    f.write(bytes(staged[offset + i * 4 + 3] for i in range(wb * hb)))
                print("level 0 A written to", alpha_pgm)
    print("PASS" if failures == 0 else "FAIL: %d mismatching block(s)" % failures)
    return 0 if failures == 0 else 1


def self_test(directory):
    # A synthetic 64x8 tiled 4-byte 8in32 texture through this script's own formulas, then one flipped staged byte:
    # checks the parsing and the comparison, not the formulas (those are cross-checked against Xenia elsewhere).
    if os.listdir(directory):
        print("self-test needs an empty directory")
        return 2
    prefix = os.path.join(directory, "texdump_00100000_0")
    w, h = 64, 8
    extent = tiled_offset_2d(32, 0, 64, 2) + 0x1000
    guest = bytes(i % 251 for i in range(extent))
    staged = b"".join(swap(guest[tiled_offset_2d(x, y, 64, 2):][:4], 2) for y in range(h) for x in range(w))
    open(prefix + "_level0_guest.bin", "wb").write(guest)
    open(prefix + "_staged.bin", "wb").write(staged)
    open(prefix + "_info.txt", "w").write(
        "words 0 0 0 0 0 0\nformat 50 k_8_8_8_8_AS_16_16_16_16 dimension 1 size 64 8 1 tiled 1 endian 2 bytes_per_block 4 levels 1 packed_level -1\n"
        "level 0 address 00100000 x 0 y 0 blocks 64 8 depth 1 texels 64 8 pitch_blocks 64 row_pitch_bytes 256 z_stride_rows 0 extent %d "
        "staged_offset 0 staged_bytes %d\n" % (extent, len(staged)))
    ok = compare(prefix) == 0
    broken = bytearray(staged)
    broken[4 * 9 + 3] ^= 1
    open(prefix + "_staged.bin", "wb").write(broken)
    ok = ok and compare(prefix) == 1
    print("self-test", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="?")
    ap.add_argument("--rect", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--alpha-pgm")
    ap.add_argument("--self-test", metavar="DIR")
    a = ap.parse_args()
    if a.self_test:
        return self_test(a.self_test)
    if not a.prefix:
        ap.error("a dump prefix is required")
    return compare(a.prefix, a.rect, a.alpha_pgm)


if __name__ == "__main__":
    sys.exit(main())
