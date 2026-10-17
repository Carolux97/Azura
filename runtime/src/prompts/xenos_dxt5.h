// DXT5 (BC3) blocks as the Xbox 360 GPU (Xenos) stores them.
//
// A DXT5 block encodes 4x4 pixels in 16 bytes: 8 bytes of alpha (two endpoints and 3-bit indices)
// and 8 bytes of colour (two RGB565 endpoints and 2-bit indices). The 360 differs from the PC
// layout in two ways:
//   - every 16-bit word of a block is big-endian;
//   - blocks are not stored row by row but "tiled": TiledBlockOffset maps a block's (x, y) to its
//     byte offset in the texture.

#pragma once

#include <array>
#include <cstdint>

namespace azura::prompts {

using Rgba = std::array<uint8_t, 4>;
using Block = std::array<uint8_t, 16>;

// Byte offset of block (block_x, block_y) in a tiled texture whose rows are `pitch_blocks` blocks
// wide. Blocks are 16 bytes.
uint32_t TiledBlockOffset(uint32_t block_x, uint32_t block_y, uint32_t pitch_blocks);

// Decodes a block in 360 byte order into 16 pixels, row by row.
std::array<Rgba, 16> DecodeDxt5(const Block& block);

// Encodes 16 straight-alpha pixels, row by row, into a block in 360 byte order. Fully transparent
// pixels do not influence the colour endpoints.
Block EncodeDxt5(const std::array<Rgba, 16>& pixels);

}  // namespace azura::prompts
