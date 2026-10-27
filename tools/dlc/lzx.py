"""LZX decompression for Xbox 360 XMemCompress data, as Unreal Engine 3 stores it.

An Unreal Engine 3 compressed chunk (tag 0x9E2A83C1) holds blocks of up to 128 KB, each an
independent XMemCompress stream: a sequence of frames of at most 32 KB of output, each with a
header (0xFF, 16-bit output size, 16-bit input size; or just the 16-bit input size for a full 32 KB
frame). The frames' data, joined, form one LZX bitstream with a 128 KB window (2^17).

This is a plain implementation of the LZX format (Microsoft's LZX DELTA specification, the format of
cabinet files): verbatim, aligned and uncompressed blocks, Huffman trees with pretree-coded deltas,
repeated offsets R0-R2. Two Xbox differences: no Intel E8 translation in practice (the header bit is
read and honoured anyway) and no padding byte after an uncompressed block of odd length.
"""

import struct

TAG = 0x9E2A83C1
MIN_MATCH = 2
NUM_CHARS = 256
PRETREE_SIZE = 20
ALIGNED_SIZE = 8
SECONDARY_LENGTHS = 249
FRAME = 0x8000

# Position slots: extra bits (0 for slots 0-3, then (slot - 2) / 2, at most 17) and base offsets.
EXTRA_BITS = [0 if slot < 4 else min((slot - 2) >> 1, 17) for slot in range(51)]
POSITION_BASE = [0] * 51
for _slot in range(1, 51):
    POSITION_BASE[_slot] = POSITION_BASE[_slot - 1] + (1 << EXTRA_BITS[_slot - 1])


class LzxError(Exception):
    pass


class _Bits:
    """MSB-first bit reader over 16-bit little-endian words."""

    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.buffer = 0
        self.left = 0

    def _fill(self, n):
        while self.left < n:
            if self.pos + 1 < len(self.data):
                word = self.data[self.pos] | (self.data[self.pos + 1] << 8)
            elif self.pos < len(self.data):
                word = self.data[self.pos]
            else:
                word = 0
            self.pos += 2
            self.buffer = (self.buffer << 16) | word
            self.left += 16

    def peek(self, n):
        self._fill(n)
        return (self.buffer >> (self.left - n)) & ((1 << n) - 1)

    def skip(self, n):
        self._fill(n)
        self.left -= n
        self.buffer &= (1 << self.left) - 1

    def read(self, n):
        if n == 0:
            return 0
        value = self.peek(n)
        self.skip(n)
        return value

    def align(self):
        """Drops the bits left in the current 16-bit word."""
        self.left -= self.left % 16
        self.buffer &= (1 << self.left) - 1

    def raw_bytes(self, n):
        """Reads n bytes directly (uncompressed block data), after align()."""
        # Bits still buffered come first (whole 16-bit words, little-endian).
        out = bytearray()
        while self.left >= 16 and n:
            word = (self.buffer >> (self.left - 16)) & 0xFFFF
            self.left -= 16
            self.buffer &= (1 << self.left) - 1
            for b in (word & 0xFF, word >> 8):
                if n:
                    out.append(b)
                    n -= 1
        out += self.data[self.pos:self.pos + n]
        self.pos += n
        return bytes(out)


class _Huffman:
    """Canonical Huffman decoder from code lengths."""

    def __init__(self, lengths):
        self.lengths = lengths
        self.max_len = max(lengths) if any(lengths) else 0
        self.table = {}
        code = 0
        for length in range(1, self.max_len + 1):
            for symbol, l in enumerate(lengths):
                if l == length:
                    self.table[(length, code)] = symbol
                    code += 1
            code <<= 1

    def decode(self, bits):
        code = 0
        for length in range(1, self.max_len + 1):
            code = (code << 1) | bits.read(1)
            symbol = self.table.get((length, code))
            if symbol is not None:
                return symbol
        raise LzxError('bad Huffman code')


class _Decoder:
    def __init__(self, window_bits=17):
        self.window_size = 1 << window_bits
        self.position_slots = 34 if window_bits == 17 else {15: 30, 16: 32, 18: 36, 19: 38, 20: 42, 21: 50}[window_bits]
        self.main_lengths = [0] * (NUM_CHARS + self.position_slots * 8)
        self.length_lengths = [0] * SECONDARY_LENGTHS
        self.r = [1, 1, 1]
        self.header_read = False

    def _read_lengths(self, bits, lengths, first, last):
        pre = _Huffman([bits.read(4) for _ in range(PRETREE_SIZE)])
        i = first
        while i < last:
            symbol = pre.decode(bits)
            if symbol == 17:
                run = bits.read(4) + 4
                for _ in range(run):
                    lengths[i] = 0; i += 1
            elif symbol == 18:
                run = bits.read(5) + 20
                for _ in range(run):
                    lengths[i] = 0; i += 1
            elif symbol == 19:
                run = bits.read(1) + 4
                symbol = pre.decode(bits)
                value = (lengths[i] - symbol) % 17
                for _ in range(run):
                    lengths[i] = value; i += 1
            else:
                lengths[i] = (lengths[i] - symbol) % 17
                i += 1

    def _read_block_header(self, bits):
        self.block_type = bits.read(3)
        self.block_remaining = (bits.read(16) << 8) | bits.read(8)
        if self.block_type == 2:  # aligned offsets block
            self.aligned = _Huffman([bits.read(3) for _ in range(ALIGNED_SIZE)])
        if self.block_type in (1, 2):  # verbatim or aligned
            self._read_lengths(bits, self.main_lengths, 0, NUM_CHARS)
            self._read_lengths(bits, self.main_lengths, NUM_CHARS, len(self.main_lengths))
            self.main = _Huffman(self.main_lengths)
            self._read_lengths(bits, self.length_lengths, 0, SECONDARY_LENGTHS)
            self.length = _Huffman(self.length_lengths) if any(self.length_lengths) else None
        elif self.block_type == 3:  # uncompressed
            # 1 to 16 bits of padding up to the next 16-bit word, then R0-R2.
            if bits.left == 0:
                bits.read(16)
            bits.left = 0
            bits.buffer = 0
            self.r = list(struct.unpack('<III', bits.raw_bytes(12)))
        else:
            raise LzxError(f'bad block type {self.block_type}')

    def _match(self, bits, symbol, out):
        symbol -= NUM_CHARS
        match_length = symbol & 7
        if match_length == 7:
            match_length += self.length.decode(bits)
        match_length += MIN_MATCH
        slot = symbol >> 3
        if slot > 2:
            extra = EXTRA_BITS[slot]
            if self.block_type == 2 and extra >= 3:
                offset = POSITION_BASE[slot] - 2 + (bits.read(extra - 3) << 3) + self.aligned.decode(bits)
            else:
                offset = POSITION_BASE[slot] - 2 + bits.read(extra)
            self.r = [offset, self.r[0], self.r[1]]
        elif slot == 0:
            offset = self.r[0]
        elif slot == 1:
            offset = self.r[1]
            self.r = [offset, self.r[0], self.r[2]]
        else:
            offset = self.r[2]
            self.r = [offset, self.r[1], self.r[0]]
        start = len(out) - offset
        if start < 0:
            raise LzxError('match before the start of the stream')
        for k in range(match_length):
            out.append(out[start + k])
        return match_length

    def decompress(self, data, out_size):
        bits = _Bits(data)
        out = bytearray()
        if not self.header_read:
            if bits.read(1):
                bits.read(32)  # Intel E8 translation size: Xbox data does not use it
            self.header_read = True
        self.block_remaining = 0
        while len(out) < out_size:
            frame_end = min((len(out) // FRAME + 1) * FRAME, out_size)
            while len(out) < frame_end:
                if self.block_remaining == 0:
                    self._read_block_header(bits)
                if self.block_type == 3:
                    n = min(self.block_remaining, frame_end - len(out))
                    out += bits.raw_bytes(n)
                    self.block_remaining -= n
                    # Xbox streams have no padding byte after an odd-sized uncompressed block.
                    continue
                while self.block_remaining > 0 and len(out) < frame_end:
                    symbol = self.main.decode(bits)
                    if symbol < NUM_CHARS:
                        out.append(symbol)
                        self.block_remaining -= 1
                    else:
                        self.block_remaining -= self._match(bits, symbol, out)
            bits.align()  # each frame's data ends on a 16-bit word
        return bytes(out[:out_size])


def xmem_frames(block):
    """Joins the LZX data of an XMemCompress stream's frames."""
    data = bytearray()
    p = 0
    while p + 2 <= len(block):
        if block[p] == 0xFF:
            size = (block[p + 3] << 8) | block[p + 4]
            p += 5
        else:
            size = (block[p] << 8) | block[p + 1]
            p += 2
        if size == 0 or p + size > len(block):
            break
        data += block[p:p + size]
        p += size
    return bytes(data)


def decompress_chunk(data, offset=0, max_output=None):
    """Decompresses the UE3 compressed chunk at `offset` (up to `max_output` bytes, whole blocks)."""
    tag, block_size, _compressed, uncompressed = struct.unpack_from('>IIII', data, offset)
    if tag != TAG:
        raise LzxError('not an Unreal Engine 3 compressed chunk')
    blocks = (uncompressed + block_size - 1) // block_size
    sizes = [struct.unpack_from('>II', data, offset + 16 + 8 * i) for i in range(blocks)]
    p = offset + 16 + 8 * blocks
    out = bytearray()
    for compressed_size, size in sizes:
        out += _Decoder().decompress(xmem_frames(data[p:p + compressed_size]), size)
        p += compressed_size
        if max_output is not None and len(out) >= max_output:
            break
    return bytes(out)
