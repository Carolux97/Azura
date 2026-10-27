// LZX decompression of Xbox 360 XMemCompress data. See lzx.h.
//
// A plain implementation of the LZX format (Microsoft's LZX DELTA specification, the format of
// cabinet files): verbatim, aligned and uncompressed blocks, Huffman trees with pretree-coded deltas,
// repeated offsets R0-R2.

#include "lzx.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace azura::dlc {
namespace {

constexpr uint32_t kMinMatch = 2;
constexpr uint32_t kNumChars = 256;
constexpr uint32_t kPretreeSize = 20;
constexpr uint32_t kAlignedSize = 8;
constexpr uint32_t kSecondaryLengths = 249;
constexpr uint32_t kFrame = 0x8000;
constexpr uint32_t kChunkBlockSize = 0x20000;
constexpr uint32_t kPositionSlots = 34;  // for the 2^17 window

enum BlockType : uint32_t { kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

// Position slots: extra bits (0 for slots 0-3, then (slot - 2) / 2, at most 17) and base offsets.
struct PositionTables {
  std::array<uint32_t, 51> extra_bits{};
  std::array<uint32_t, 51> base{};
  PositionTables() {
    for (uint32_t slot = 0; slot < 51; ++slot) extra_bits[slot] = slot < 4 ? 0 : std::min((slot - 2) >> 1, 17u);
    for (uint32_t slot = 1; slot < 51; ++slot) base[slot] = base[slot - 1] + (1u << extra_bits[slot - 1]);
  }
};
const PositionTables kPositions;

struct DecodeError {};

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBe16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void StoreBe32(std::vector<uint8_t>& out, uint32_t v) {
  StoreBe16(out, v >> 16);
  StoreBe16(out, v);
}

void StoreLe16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v));
  out.push_back(uint8_t(v >> 8));
}

void StoreLe32(std::vector<uint8_t>& out, uint32_t v) {
  StoreLe16(out, v);
  StoreLe16(out, v >> 16);
}

// MSB-first bit reader over 16-bit little-endian words. After every read fewer than 16 bits stay
// buffered, so the buffer never holds more than 47.
class Bits {
 public:
  explicit Bits(const std::vector<uint8_t>& data) : data_(data) {}

  uint32_t Read(uint32_t n) {
    if (n == 0) return 0;
    Fill(n);
    left_ -= n;
    const uint32_t value = uint32_t(buffer_ >> left_) & uint32_t((uint64_t(1) << n) - 1);
    buffer_ &= (uint64_t(1) << left_) - 1;
    return value;
  }

  // Drops the bits left in the current 16-bit word.
  void Align() {
    left_ -= left_ % 16;
    buffer_ &= (uint64_t(1) << left_) - 1;
  }

  void Discard() {
    left_ = 0;
    buffer_ = 0;
  }

  uint32_t left() const { return left_; }

  // Copies n bytes directly (uncompressed block data); only called with no bits buffered.
  void RawBytes(uint32_t n, std::vector<uint8_t>& out) {
    for (uint32_t i = 0; i < n; ++i) out.push_back(pos_ + i < data_.size() ? data_[pos_ + i] : 0);
    pos_ += n;
  }

 private:
  void Fill(uint32_t n) {
    while (left_ < n) {
      uint32_t word = 0;
      if (pos_ + 1 < data_.size()) {
        word = data_[pos_] | (uint32_t(data_[pos_ + 1]) << 8);
      } else if (pos_ < data_.size()) {
        word = data_[pos_];
      }
      pos_ += 2;
      buffer_ = (buffer_ << 16) | word;
      left_ += 16;
    }
  }

  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
  uint64_t buffer_ = 0;
  uint32_t left_ = 0;
};

// Canonical Huffman decoder from code lengths: symbols of each length take consecutive codes, in
// symbol order, and the first code of a length is twice the one after the previous length's last.
class Huffman {
 public:
  Huffman() = default;
  explicit Huffman(const std::vector<uint8_t>& lengths) {
    for (uint8_t l : lengths) max_len_ = std::max<uint32_t>(max_len_, l);
    uint32_t code = 0;
    for (uint32_t length = 1; length <= max_len_; ++length) {
      first_[length] = code;
      offset_[length] = uint32_t(sorted_.size());
      for (uint32_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] == length) sorted_.push_back(symbol);
      }
      count_[length] = uint32_t(sorted_.size()) - offset_[length];
      code = (code + count_[length]) << 1;
    }
  }

  bool empty() const { return sorted_.empty(); }

  uint32_t Decode(Bits& bits) const {
    uint32_t code = 0;
    for (uint32_t length = 1; length <= max_len_; ++length) {
      code = (code << 1) | bits.Read(1);
      if (code - first_[length] < count_[length]) return sorted_[offset_[length] + code - first_[length]];
    }
    throw DecodeError{};
  }

 private:
  uint32_t max_len_ = 0;
  std::array<uint32_t, 17> first_{};
  std::array<uint32_t, 17> offset_{};
  std::array<uint32_t, 17> count_{};
  std::vector<uint32_t> sorted_;
};

class Decoder {
 public:
  // Decompresses one XMemCompress stream's LZX data into `out_size` bytes.
  std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, uint32_t out_size) {
    Bits bits(data);
    std::vector<uint8_t> out;
    out.reserve(out_size);
    if (bits.Read(1)) bits.Read(32);  // Intel E8 translation size: Xbox data does not use it
    block_remaining_ = 0;
    while (out.size() < out_size) {
      const size_t frame_end = std::min<size_t>((out.size() / kFrame + 1) * kFrame, out_size);
      while (out.size() < frame_end) {
        if (block_remaining_ == 0) ReadBlockHeader(bits);
        if (block_type_ == kUncompressed) {
          const uint32_t n = uint32_t(std::min<size_t>(block_remaining_, frame_end - out.size()));
          bits.RawBytes(n, out);
          block_remaining_ -= n;
          // Xbox streams have no padding byte after an odd-sized uncompressed block.
          continue;
        }
        while (block_remaining_ > 0 && out.size() < frame_end) {
          const uint32_t symbol = main_.Decode(bits);
          if (symbol < kNumChars) {
            out.push_back(uint8_t(symbol));
            block_remaining_ -= 1;
          } else {
            block_remaining_ -= std::min(block_remaining_, Match(bits, symbol, out));
          }
        }
      }
      bits.Align();  // each frame's data ends on a 16-bit word
    }
    out.resize(out_size);
    return out;
  }

 private:
  void ReadLengths(Bits& bits, std::vector<uint8_t>& lengths, size_t first, size_t last) {
    std::vector<uint8_t> pre_lengths(kPretreeSize);
    for (auto& l : pre_lengths) l = uint8_t(bits.Read(4));
    const Huffman pre(pre_lengths);
    size_t i = first;
    auto put = [&](uint8_t value) {
      if (i >= last) throw DecodeError{};
      lengths[i++] = value;
    };
    while (i < last) {
      const uint32_t symbol = pre.Decode(bits);
      if (symbol == 17) {
        for (uint32_t run = bits.Read(4) + 4; run > 0; --run) put(0);
      } else if (symbol == 18) {
        for (uint32_t run = bits.Read(5) + 20; run > 0; --run) put(0);
      } else if (symbol == 19) {
        const uint32_t run = bits.Read(1) + 4;
        const uint32_t delta = pre.Decode(bits);
        if (i >= last) throw DecodeError{};
        const uint8_t value = uint8_t((lengths[i] + 17 - delta) % 17);
        for (uint32_t k = 0; k < run; ++k) put(value);
      } else {
        put(uint8_t((lengths[i] + 17 - symbol) % 17));
      }
    }
  }

  void ReadBlockHeader(Bits& bits) {
    block_type_ = bits.Read(3);
    block_remaining_ = (bits.Read(16) << 8) | bits.Read(8);
    if (block_type_ == kAligned) {
      std::vector<uint8_t> aligned(kAlignedSize);
      for (auto& l : aligned) l = uint8_t(bits.Read(3));
      aligned_ = Huffman(aligned);
    }
    if (block_type_ == kVerbatim || block_type_ == kAligned) {
      ReadLengths(bits, main_lengths_, 0, kNumChars);
      ReadLengths(bits, main_lengths_, kNumChars, main_lengths_.size());
      main_ = Huffman(main_lengths_);
      ReadLengths(bits, length_lengths_, 0, kSecondaryLengths);
      length_ = Huffman(length_lengths_);
    } else if (block_type_ == kUncompressed) {
      // 1 to 16 bits of padding up to the next 16-bit word, then R0-R2.
      if (bits.left() == 0) bits.Read(16);
      bits.Discard();
      std::vector<uint8_t> raw;
      bits.RawBytes(12, raw);
      for (int k = 0; k < 3; ++k) {
        r_[k] = raw[k * 4] | (uint32_t(raw[k * 4 + 1]) << 8) | (uint32_t(raw[k * 4 + 2]) << 16) |
                (uint32_t(raw[k * 4 + 3]) << 24);
      }
    } else {
      throw DecodeError{};
    }
  }

  uint32_t Match(Bits& bits, uint32_t symbol, std::vector<uint8_t>& out) {
    symbol -= kNumChars;
    uint32_t match_length = symbol & 7;
    if (match_length == 7) {
      if (length_.empty()) throw DecodeError{};
      match_length += length_.Decode(bits);
    }
    match_length += kMinMatch;
    const uint32_t slot = symbol >> 3;
    uint32_t offset;
    if (slot > 2) {
      const uint32_t extra = kPositions.extra_bits[slot];
      if (block_type_ == kAligned && extra >= 3) {
        offset = kPositions.base[slot] - 2 + (bits.Read(extra - 3) << 3) + aligned_.Decode(bits);
      } else {
        offset = kPositions.base[slot] - 2 + bits.Read(extra);
      }
      r_ = {offset, r_[0], r_[1]};
    } else if (slot == 0) {
      offset = r_[0];
    } else if (slot == 1) {
      offset = r_[1];
      r_ = {offset, r_[0], r_[2]};
    } else {
      offset = r_[2];
      r_ = {offset, r_[1], r_[0]};
    }
    if (offset > out.size()) throw DecodeError{};  // a match before the start of the stream
    const size_t start = out.size() - offset;
    for (uint32_t k = 0; k < match_length; ++k) out.push_back(out[start + k]);
    return match_length;
  }

  std::vector<uint8_t> main_lengths_ = std::vector<uint8_t>(kNumChars + kPositionSlots * 8);
  std::vector<uint8_t> length_lengths_ = std::vector<uint8_t>(kSecondaryLengths);
  std::array<uint32_t, 3> r_{1, 1, 1};
  uint32_t block_type_ = 0;
  uint32_t block_remaining_ = 0;
  Huffman main_, length_, aligned_;
};

// Joins the LZX data of an XMemCompress stream's frames.
std::vector<uint8_t> XmemFrames(const uint8_t* block, size_t size) {
  std::vector<uint8_t> data;
  size_t p = 0;
  while (p + 2 <= size) {
    uint32_t frame_size;
    if (block[p] == 0xFF) {
      if (p + 5 > size) break;
      frame_size = (uint32_t(block[p + 3]) << 8) | block[p + 4];
      p += 5;
    } else {
      frame_size = (uint32_t(block[p]) << 8) | block[p + 1];
      p += 2;
    }
    if (frame_size == 0 || p + frame_size > size) break;
    data.insert(data.end(), block + p, block + p + frame_size);
    p += frame_size;
  }
  return data;
}

}  // namespace

std::optional<std::vector<uint8_t>> DecompressChunk(const std::vector<uint8_t>& data) {
  if (data.size() < 16 || LoadBe32(data.data()) != kPackageTag) return std::nullopt;
  const uint32_t block_size = LoadBe32(data.data() + 4);
  const uint32_t uncompressed = LoadBe32(data.data() + 12);
  if (block_size == 0) return std::nullopt;
  const size_t blocks = (size_t(uncompressed) + block_size - 1) / block_size;
  if (16 + 8 * blocks > data.size()) return std::nullopt;
  std::vector<uint8_t> out;
  out.reserve(uncompressed);
  size_t p = 16 + 8 * blocks;
  try {
    for (size_t i = 0; i < blocks; ++i) {
      const uint32_t compressed_size = LoadBe32(data.data() + 16 + 8 * i);
      const uint32_t size = LoadBe32(data.data() + 20 + 8 * i);
      if (p + compressed_size > data.size()) return std::nullopt;
      const std::vector<uint8_t> block = Decoder().Decompress(XmemFrames(data.data() + p, compressed_size), size);
      out.insert(out.end(), block.begin(), block.end());
      p += compressed_size;
    }
  } catch (const DecodeError&) {
    return std::nullopt;
  }
  return out;
}

std::vector<uint8_t> PackStored(const std::vector<uint8_t>& data) {
  std::vector<std::pair<size_t, size_t>> blocks;  // (start, size)
  for (size_t start = 0; start < data.size(); start += kChunkBlockSize) {
    blocks.emplace_back(start, std::min<size_t>(kChunkBlockSize, data.size() - start));
  }
  if (blocks.empty()) blocks.emplace_back(0, 0);
  std::vector<std::vector<uint8_t>> streams;
  for (const auto& [block_start, block_size] : blocks) {
    // LZX bits, most significant first in 16-bit little-endian words: no Intel E8 translation (1
    // bit), block type 3 = uncompressed (3 bits), length (24 bits), padding to 32 bits; then R0-R2.
    // XMemCompress splits the stream into frames of 32 KB of output.
    const uint32_t value = ((3u << 24) | uint32_t(block_size)) << 4;
    std::vector<uint8_t> stream;
    StoreLe16(stream, value >> 16);
    StoreLe16(stream, value & 0xFFFF);
    for (int k = 0; k < 3; ++k) StoreLe32(stream, 1);
    const size_t header_size = stream.size();
    stream.insert(stream.end(), data.begin() + block_start, data.begin() + block_start + block_size);
    std::vector<uint8_t> out;
    size_t pos = 0;
    for (size_t start = 0; start < block_size; start += kFrame) {
      const size_t size = std::min<size_t>(kFrame, block_size - start);
      const size_t take = size + (start == 0 ? header_size : 0);
      if (size == kFrame) {
        StoreBe16(out, uint32_t(take));
      } else {
        out.push_back(0xFF);
        StoreBe16(out, uint32_t(size));
        StoreBe16(out, uint32_t(take));
      }
      out.insert(out.end(), stream.begin() + pos, stream.begin() + pos + take);
      pos += take;
    }
    out.insert(out.end(), 9, 0);  // end of stream (an empty frame) and padding, as XMemCompress writes them
    streams.push_back(std::move(out));
  }
  std::vector<uint8_t> head;
  size_t total = 0;
  for (const auto& s : streams) total += s.size();
  StoreBe32(head, kPackageTag);
  StoreBe32(head, kChunkBlockSize);
  StoreBe32(head, uint32_t(total));
  StoreBe32(head, uint32_t(data.size()));
  for (size_t i = 0; i < blocks.size(); ++i) {
    StoreBe32(head, uint32_t(streams[i].size()));
    StoreBe32(head, uint32_t(blocks[i].second));
  }
  for (const auto& s : streams) head.insert(head.end(), s.begin(), s.end());
  return head;
}

}  // namespace azura::dlc
