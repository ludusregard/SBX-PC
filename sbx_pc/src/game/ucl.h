// UCL decompression as the archive uses it: UCL's NRV2B with 32-bit
// little-endian bit words, in `uclpack`'s stream layout. Works on host memory
// only, so the same code serves the game and the mod tools.
//
// A stream (`docs/game_data.md`, "The archives"):
//
//   18-byte header (only its last word, the first block's size, is read)
//   blocks: unpacked size (u32 BE), stored size (u32 BE), stored bytes
//   an unpacked size of 0 ends the stream
//
// A block stored no smaller than its unpacked size is a plain copy.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace ucl {

enum class BlockStatus {
  kOk,                // The input was used up exactly.
  kInputOverrun,      // The input ran out mid-block.
  kInputNotConsumed,  // The block ended before the input did.
  kOutputOverrun,     // The block would write past `out`.
};

struct BlockResult {
  BlockStatus status;
  size_t written;  // Bytes written to `out`.
  size_t read;     // Bytes read from `in`.
};

// Unpacks one NRV2B block from `in` into `out`.
BlockResult UnpackBlock(std::span<const uint8_t> in, std::span<uint8_t> out);

// The stream's length in bytes (header, blocks and the end marker), found by
// walking its block headers; 0 when `max_length` ends before the stream does.
size_t StreamLength(const uint8_t* stream, size_t max_length);

// How a stored block is copied out (std::memcpy when not given). The game
// copies through its memcpy, which the renderer watches.
using CopyFunction = std::function<void(uint8_t* to, const uint8_t* from, size_t size)>;

// Unpacks a whole stream into `out`. True when the blocks add up to exactly
// `out.size()` bytes; false as soon as they would pass it (nothing past it is
// written) or when the stream is cut short.
bool Unpack(std::span<const uint8_t> stream, std::span<uint8_t> out,
            const CopyFunction& copy = nullptr);

}  // namespace ucl
