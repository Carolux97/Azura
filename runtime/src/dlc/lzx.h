// LZX decompression of Xbox 360 XMemCompress data, as Unreal Engine 3 stores its packages.
//
// An Unreal Engine 3 compressed chunk (tag 0x9E2A83C1) holds blocks of up to 128 KB, each an
// independent XMemCompress stream: frames of at most 32 KB of output, each with a header (0xFF,
// 16-bit output size, 16-bit input size; or just the 16-bit input size for a full 32 KB frame). The
// frames' data, joined, form one LZX bitstream with a 128 KB window.
//
// The SDK's decoder (libmspack, for executables) cannot read these: it expects a padding byte after
// an uncompressed block of odd length, which Xbox streams do not have. This is the decoder of
// tools/dlc/lzx.py, which Azura's script patch tools were checked with.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace azura::dlc {

// Tag of an Unreal Engine 3 compressed chunk (and of a package summary).
constexpr uint32_t kPackageTag = 0x9E2A83C1;

// Decompresses the compressed chunk at the start of `data`; nothing when it is not one or is
// damaged.
std::optional<std::vector<uint8_t>> DecompressChunk(const std::vector<uint8_t>& data);

// `data` as a compressed chunk of stored (uncompressed) LZX blocks, which the engine reads like any
// other LZX data.
std::vector<uint8_t> PackStored(const std::vector<uint8_t>& data);

}  // namespace azura::dlc
