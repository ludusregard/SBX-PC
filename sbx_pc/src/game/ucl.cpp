// UCL decompression: see ucl.h.

#include "game/ucl.h"

#include <cstring>

namespace ucl {
namespace {

// The stream's sizes are big-endian and may sit at any byte.
uint32_t ReadBigEndian(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | uint32_t(at[3]);
}

constexpr size_t kHeaderSize = 18;  // The first block's unpacked size follows.

// NRV2B's input: literal bytes, and control bits taken one at a time from
// 32-bit little-endian words, highest bit first.
class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> in) : in_(in) {}

  bool Bit() {
    if (bits_left_ == 0) {
      if (position_ + 4 > in_.size()) {
        overrun_ = true;
        return false;
      }
      const uint8_t* at = &in_[position_];
      word_ = uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 |
              uint32_t(at[3]) << 24;
      position_ += 4;
      bits_left_ = 32;
    }
    bits_left_--;
    return (word_ >> bits_left_) & 1;
  }

  uint8_t Byte() {
    if (position_ >= in_.size()) {
      overrun_ = true;
      return 0;
    }
    return in_[position_++];
  }

  bool overrun() const { return overrun_; }
  size_t position() const { return position_; }
  size_t size() const { return in_.size(); }

 private:
  std::span<const uint8_t> in_;
  size_t position_ = 0;
  uint32_t word_ = 0;
  int bits_left_ = 0;
  bool overrun_ = false;
};

}  // namespace

BlockResult UnpackBlock(std::span<const uint8_t> in, std::span<uint8_t> out) {
  BitReader reader(in);
  size_t written = 0;
  uint32_t last_offset = 1;

  auto result = [&](BlockStatus status) { return BlockResult{status, written, reader.position()}; };

  for (;;) {
    // A 1 bit: one literal byte.
    while (reader.Bit()) {
      if (written >= out.size()) {
        return result(BlockStatus::kOutputOverrun);
      }
      out[written++] = reader.Byte();
    }
    if (reader.overrun()) {
      return result(BlockStatus::kInputOverrun);
    }

    // A match. Its offset's high part, as an Elias-gamma-like code; 2 means
    // "the same offset as the last match".
    uint32_t offset_code = 1;
    do {
      offset_code = offset_code * 2 + reader.Bit();
    } while (!reader.Bit());
    if (offset_code != 2) {
      const uint32_t offset = ((offset_code - 3) << 8) + reader.Byte();
      if (offset == 0xFFFFFFFFu) {
        break;  // The end marker.
      }
      last_offset = offset + 1;
    }

    // Its length: two bits, or a longer code when both are 0.
    uint32_t length = reader.Bit();
    length = length * 2 + reader.Bit();
    if (length == 0) {
      length = 1;
      do {
        length = length * 2 + reader.Bit();
      } while (!reader.Bit());
      length += 2;
    }
    // Far matches are one byte longer; every match copies at least two.
    length += (last_offset > 0xD00 ? 1 : 0) + 1;

    if (reader.overrun()) {
      return result(BlockStatus::kInputOverrun);
    }
    if (last_offset > written || written + length > out.size()) {
      return result(BlockStatus::kOutputOverrun);
    }
    // Byte by byte: a match may overlap what it's copying.
    for (uint32_t i = 0; i < length; i++, written++) {
      out[written] = out[written - last_offset];
    }
  }

  if (reader.overrun()) {
    return result(BlockStatus::kInputOverrun);
  }
  return result(reader.position() == reader.size() ? BlockStatus::kOk
                                                   : BlockStatus::kInputNotConsumed);
}

size_t StreamLength(const uint8_t* stream, size_t max_length) {
  size_t at = kHeaderSize;
  for (;;) {
    if (at + 4 > max_length) {
      return 0;
    }
    const uint32_t unpacked = ReadBigEndian(stream + at);
    at += 4;
    if (unpacked == 0) {
      return at;
    }
    if (at + 4 > max_length) {
      return 0;
    }
    const uint32_t stored = ReadBigEndian(stream + at);
    at += 4 + size_t(stored);
  }
}

bool Unpack(std::span<const uint8_t> stream, std::span<uint8_t> out, const CopyFunction& copy) {
  size_t at = kHeaderSize;
  size_t written = 0;
  for (;;) {
    if (at + 4 > stream.size()) {
      return false;
    }
    const uint32_t unpacked = ReadBigEndian(&stream[at]);
    at += 4;
    if (unpacked == 0) {
      break;
    }
    if (at + 4 > stream.size()) {
      return false;
    }
    const uint32_t stored = ReadBigEndian(&stream[at]);
    at += 4;
    if (at + stored > stream.size()) {
      return false;
    }
    const std::span<const uint8_t> data = stream.subspan(at, stored);
    at += stored;

    if (stored < unpacked) {
      // Compressed. The game counts the block's stated size, not what the
      // decoder wrote.
      if (written + unpacked > out.size()) {
        return false;
      }
      UnpackBlock(data, out.subspan(written, unpacked));
      written += unpacked;
    } else {
      // Stored as is.
      if (written + stored > out.size()) {
        return false;
      }
      if (copy) {
        copy(out.data() + written, data.data(), stored);
      } else {
        std::memcpy(out.data() + written, data.data(), stored);
      }
      written += stored;
    }
  }
  return written == out.size();
}

}  // namespace ucl
