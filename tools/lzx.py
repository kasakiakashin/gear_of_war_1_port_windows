#!/usr/bin/env python3
"""
lzx.py - Pure-Python LZX decompressor (Microsoft LZX, as used by XEX2 "normal"
compression). Written from scratch for the Gears of War port project.

Bitstream model
---------------
* Input is consumed as 16-bit little-endian words; bits are taken MSB-first
  from each word.
* Output is produced in 32 KiB frames. After each frame the bitstream is
  re-aligned to the next 16-bit word boundary.
* Stream header: 1 bit "E8 translation"; if set, 32 bits of translation size.
* Block header: 3-bit type (1 verbatim, 2 aligned-offset, 3 uncompressed) +
  24-bit uncompressed length (read as 16 + 8 bits).
* Huffman trees are canonical, max code length 16. Main/length tree code
  lengths are delta-coded through a 20-symbol "pretree" and persist between
  blocks (deltas are relative to the previous block's lengths).
* Repeated-offset registers R0/R1/R2 start at 1.

The decoder keeps the whole output in one linear buffer instead of a circular
window, which is valid because XEX decompression always produces the entire
image in one pass (match offsets never exceed the window size).
"""

NUM_CHARS = 256
PRETREE_SYMS = 20
ALIGNED_SYMS = 8
NUM_PRIMARY_LENGTHS = 7
NUM_SECONDARY_LENGTHS = 249
MIN_MATCH = 2
FRAME_SIZE = 0x8000
POSITION_SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50, 22: 66, 23: 98, 24: 162, 25: 290}

BLOCK_VERBATIM = 1
BLOCK_ALIGNED = 2
BLOCK_UNCOMPRESSED = 3


def _build_tables():
    extra = []
    j = 0
    for i in range(0, 290, 2):
        extra += [j, j]
        if i != 0 and j < 17:
            j += 1
    extra = extra[:290]
    base = []
    j = 0
    for i in range(290):
        base.append(j)
        j += 1 << extra[i]
    return extra, base


EXTRA_BITS, POSITION_BASE = _build_tables()


class LzxError(Exception):
    pass


def make_table(lengths, nsyms):
    """Build a 16-bit direct lookup table for a canonical Huffman code.
    Returns a list of 65536 symbols (or None if all lengths are zero)."""
    table = [0] * 65536
    code = 0
    filled = 0
    any_sym = False
    for L in range(1, 17):
        shift = 16 - L
        span = 1 << shift
        for sym in range(nsyms):
            if lengths[sym] == L:
                start = code << shift
                if start + span > 65536:
                    raise LzxError("over-subscribed Huffman table")
                table[start:start + span] = [sym] * span
                code += 1
                filled += span
                any_sym = True
        code <<= 1
    if not any_sym:
        return None
    if filled != 65536:
        # incomplete table: only legal when exactly one code is present
        if filled != 32768:
            raise LzxError("incomplete Huffman table")
    return table


class _Bits:
    """MSB-first bit reader over 16-bit LE words. Fields are public so the
    hot loop in decompress() can cache them in locals."""

    __slots__ = ("d", "p", "bb", "bl")

    def __init__(self, data):
        self.d = bytes(data) + b"\x00" * 64  # zero padding past EOF, like libmspack
        self.p = 0
        self.bb = 0
        self.bl = 0

    def ensure(self, n):
        d = self.d
        while self.bl < n:
            self.bb = (self.bb << 16) | d[self.p] | (d[self.p + 1] << 8)
            self.p += 2
            self.bl += 16

    def read(self, n):
        if n == 0:
            return 0
        self.ensure(n)
        self.bl -= n
        v = self.bb >> self.bl
        self.bb &= (1 << self.bl) - 1
        return v

    def sym(self, table, lens):
        self.ensure(16)
        s = table[(self.bb >> (self.bl - 16)) & 0xFFFF]
        self.bl -= lens[s]
        self.bb &= (1 << self.bl) - 1
        return s


def _read_lengths(br, lens, first, last):
    pre_len = [br.read(4) for _ in range(PRETREE_SYMS)]
    pre = make_table(pre_len, PRETREE_SYMS)
    if pre is None:
        raise LzxError("empty pretree")
    x = first
    while x < last:
        z = br.sym(pre, pre_len)
        if z == 17:
            y = br.read(4) + 4
            lens[x:x + y] = [0] * y
            x += y
        elif z == 18:
            y = br.read(5) + 20
            lens[x:x + y] = [0] * y
            x += y
        elif z == 19:
            y = br.read(1) + 4
            z = br.sym(pre, pre_len)
            z = lens[x] - z
            if z < 0:
                z += 17
            lens[x:x + y] = [z] * y
            x += y
        else:
            z = lens[x] - z
            if z < 0:
                z += 17
            lens[x] = z
            x += 1
    # runs may overshoot 'last'; the length arrays carry 64 slots of slack for that


def decompress(data, out_len, window_size):
    window_bits = window_size.bit_length() - 1
    if (1 << window_bits) != window_size or window_bits not in POSITION_SLOTS:
        raise LzxError(f"unsupported window size 0x{window_size:X}")
    num_offsets = POSITION_SLOTS[window_bits] << 3
    main_syms = NUM_CHARS + num_offsets

    br = _Bits(data)
    out = bytearray(out_len)
    main_len = [0] * (main_syms + 64)
    len_len = [0] * (NUM_SECONDARY_LENGTHS + 64)
    aligned_len = [0] * ALIGNED_SYMS
    main_tab = len_tab = aligned_tab = None
    R0 = R1 = R2 = 1
    block_type = 0
    block_len = 0
    block_rem = 0
    intel_started = False

    # stream header
    intel_filesize = 0
    if br.read(1):
        hi = br.read(16)
        lo = br.read(16)
        intel_filesize = (hi << 16) | lo

    pos = 0
    frame = 0
    e8_frames = []
    extra_bits = EXTRA_BITS
    position_base = POSITION_BASE
    while pos < out_len:
        frame_size = min(FRAME_SIZE, out_len - pos)
        frame_start = pos
        frame_end = pos + frame_size
        while pos < frame_end:
            if block_rem == 0:
                if block_type == BLOCK_UNCOMPRESSED and (block_len & 1):
                    br.p += 1  # realign after odd-sized uncompressed block
                block_type = br.read(3)
                hi = br.read(16)
                lo = br.read(8)
                block_rem = block_len = (hi << 8) | lo
                if block_type == BLOCK_ALIGNED:
                    aligned_len = [br.read(3) for _ in range(ALIGNED_SYMS)]
                    aligned_tab = make_table(aligned_len, ALIGNED_SYMS)
                if block_type in (BLOCK_VERBATIM, BLOCK_ALIGNED):
                    _read_lengths(br, main_len, 0, 256)
                    _read_lengths(br, main_len, 256, main_syms)
                    main_tab = make_table(main_len, main_syms)
                    if main_len[0xE8]:
                        intel_started = True
                    _read_lengths(br, len_len, 0, NUM_SECONDARY_LENGTHS)
                    len_tab = make_table(len_len, NUM_SECONDARY_LENGTHS)
                elif block_type == BLOCK_UNCOMPRESSED:
                    intel_started = True
                    if br.bl == 0:
                        br.ensure(16)
                    br.bl = 0
                    br.bb = 0
                    d = br.d
                    p = br.p
                    R0 = int.from_bytes(d[p:p + 4], "little")
                    R1 = int.from_bytes(d[p + 4:p + 8], "little")
                    R2 = int.from_bytes(d[p + 8:p + 12], "little")
                    br.p = p + 12
                else:
                    raise LzxError(f"bad block type {block_type} at out=0x{pos:X}")

            this_run = min(block_rem, frame_end - pos)
            block_rem -= this_run
            run_end = pos + this_run

            if block_type == BLOCK_UNCOMPRESSED:
                p = br.p
                out[pos:run_end] = br.d[p:p + this_run]
                br.p = p + this_run
                pos = run_end
                continue

            # ---- hot loop: verbatim / aligned ----
            aligned = block_type == BLOCK_ALIGNED
            d = br.d
            p = br.p
            bb = br.bb
            bl = br.bl
            mt = main_tab
            ml = main_len
            lt = len_tab
            ll = len_len
            at = aligned_tab
            al = aligned_len
            while pos < run_end:
                if bl < 16:
                    bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                    p += 2
                    bl += 16
                s = mt[(bb >> (bl - 16)) & 0xFFFF]
                bl -= ml[s]
                bb &= (1 << bl) - 1
                if s < 256:
                    out[pos] = s
                    pos += 1
                    continue
                s -= 256
                mlen = s & 7
                if mlen == 7:
                    if bl < 16:
                        bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                        p += 2
                        bl += 16
                    t = lt[(bb >> (bl - 16)) & 0xFFFF]
                    bl -= ll[t]
                    bb &= (1 << bl) - 1
                    mlen += t
                mlen += MIN_MATCH
                slot = s >> 3
                if slot == 0:
                    off = R0
                elif slot == 1:
                    off = R1
                    R1 = R0
                    R0 = off
                elif slot == 2:
                    off = R2
                    R2 = R0
                    R0 = off
                elif slot == 3:
                    off = 1
                    R2 = R1
                    R1 = R0
                    R0 = off
                else:
                    extra = 17 if slot >= 36 else extra_bits[slot]
                    off = position_base[slot] - 2
                    if aligned:
                        if extra > 3:
                            n = extra - 3
                            while bl < n:
                                bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                                p += 2
                                bl += 16
                            bl -= n
                            off += (bb >> bl) << 3
                            bb &= (1 << bl) - 1
                            if bl < 16:
                                bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                                p += 2
                                bl += 16
                            t = at[(bb >> (bl - 16)) & 0xFFFF]
                            bl -= al[t]
                            bb &= (1 << bl) - 1
                            off += t
                        elif extra == 3:
                            if bl < 16:
                                bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                                p += 2
                                bl += 16
                            t = at[(bb >> (bl - 16)) & 0xFFFF]
                            bl -= al[t]
                            bb &= (1 << bl) - 1
                            off += t
                        elif extra > 0:
                            while bl < extra:
                                bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                                p += 2
                                bl += 16
                            bl -= extra
                            off += bb >> bl
                            bb &= (1 << bl) - 1
                        else:
                            off = 1
                    else:
                        if extra:
                            while bl < extra:
                                bb = (bb << 16) | d[p] | (d[p + 1] << 8)
                                p += 2
                                bl += 16
                            bl -= extra
                            off += bb >> bl
                            bb &= (1 << bl) - 1
                    R2 = R1
                    R1 = R0
                    R0 = off
                src = pos - off
                if src < 0:
                    raise LzxError(f"match offset {off} before start at out=0x{pos:X}")
                end = pos + mlen
                if end > frame_end:
                    raise LzxError(f"match crosses frame boundary at out=0x{pos:X}")
                if off >= mlen:
                    out[pos:end] = out[src:src + mlen]
                else:
                    pat = out[src:pos]
                    out[pos:end] = (pat * (mlen // off + 1))[:mlen]
                pos = end
            br.p = p
            br.bb = bb
            br.bl = bl
            if pos > run_end:  # last match overran this run into the block remainder
                over = pos - run_end
                if over > block_rem:
                    raise LzxError("match overruns block")
                block_rem -= over

        if pos != frame_end:
            raise LzxError(f"frame {frame} size mismatch")
        # re-align bitstream to 16 bits at the end of every frame
        drop = br.bl & 15
        if drop:
            br.bl -= drop
            br.bb &= (1 << br.bl) - 1

        if intel_started and intel_filesize and frame < 32768 and frame_size > 10:
            e8_frames.append((frame_start, frame_size))
        frame += 1
    if not e8_frames:
        return bytes(out)
    # E8 translation is applied to the output copy only; the window (used for
    # match copies above) must keep the untranslated bytes.
    final = bytearray(out)
    for start, size in e8_frames:
        _e8_decode(out, final, start, size, intel_filesize)
    return bytes(final)


def _e8_decode(src, dst, start, size, filesize):
    i = start
    end = start + size - 10
    while i < end:
        if src[i] != 0xE8:
            i += 1
            continue
        abs_off = int.from_bytes(src[i + 1:i + 5], "little", signed=True)
        if -i <= abs_off < filesize:
            rel = abs_off - i if abs_off >= 0 else abs_off + filesize
            dst[i + 1:i + 5] = (rel & 0xFFFFFFFF).to_bytes(4, "little")
        i += 5
