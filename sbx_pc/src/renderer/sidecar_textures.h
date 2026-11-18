// Sidecar textures (docs/game_data.md, "Sidecar textures (SBPR)"): full-size
// textures kept beside a pack in `<id>.sbpr`, drawn in the place of their
// stand-ins in the XPR2. The game loads and uses only the stand-ins; the
// texture cache uploads a sidecar's DDS instead of the stand-in's guest data
// (src/renderer/vulkan/textures.cpp). Stand-ins are registered by
// XPR::Create's wrapper (src/hooks/sidecar.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "renderer/vulkan/vulkan_functions.h"

namespace renderer::sidecar {

// A full-size texture as uploaded: its levels, each rows of blocks packed.
struct Image {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t block_width = 1, block_height = 1, bytes_per_block = 4;
  uint32_t width = 0, height = 0, levels = 1;
  std::vector<uint8_t> data;
  std::vector<size_t> level_offsets, level_sizes;
};

// A stand-in in guest memory and where its full-size texture is.
struct Entry {
  // The stand-in, as its fetch constant gives it after XPR::Create: its
  // base address's page (physical >> 12), format and size; and a hash of
  // the start of its data (hash_size bytes, at most 4 KiB) as the pack
  // holds it, which a texture drawn from guest memory must match.
  uint32_t page = 0, guest_format = 0, width = 0, height = 0;
  uint64_t data_hash = 0;
  uint32_t hash_size = 0;
  // The full-size texture: the sidecar, its DDS's offset and size in it,
  // and what its header says.
  std::filesystem::path file;
  uint64_t offset = 0, size = 0;
  uint32_t full_width = 0, full_height = 0, levels = 1;
  std::string name;
};

// Whether sidecars are used (--sidecar_textures).
bool Enabled();

// Reads a DDS's header (from the sidecar, `size` bytes at `offset`): its
// size and level count. False if it isn't a DDS this reads.
bool ReadHeader(const std::filesystem::path& file, uint64_t offset, uint64_t size,
                uint32_t& width, uint32_t& height, uint32_t& levels, std::string& error);

// The stand-in's page, format and size from a fetch constant (host-order
// words). False if it isn't a 2D texture with a base level.
bool StandIn(const uint32_t fetch[6], uint32_t& page, uint32_t& format, uint32_t& width,
             uint32_t& height);

// The hash Entry::data_hash is: of bytes as the pack holds them, and of
// guest memory at a page.
uint64_t HashData(const uint8_t* data, uint32_t size);
uint64_t HashGuest(uint8_t* base, uint32_t page, uint32_t size);

// Any thread (XPR::Create's wrapper): registers a stand-in, replacing
// what was registered at its page.
void Register(Entry entry);

// The entry for a fetch constant: a stand-in of its format and size
// registered at its address or near it (the nearest), whose data (with
// `base`) is what the memory at the fetch constant's address holds. Null
// with --sidecar_textures off.
std::shared_ptr<const Entry> Match(uint8_t* base, const uint32_t fetch[6]);

// The entry's full-size texture, read from its sidecar once and kept while
// a texture uses it. Null (logged once) if it can't be read.
std::shared_ptr<const Image> Load(const Entry& entry);

// Pages (physical >> 12) registered since the last call: a texture the cache
// already made from guest memory there is made again (render thread).
std::vector<uint32_t> TakeRegistered();

// Bumped by each registration and by --sidecar_textures changing: answers
// worked out before may be stale.
uint64_t Generation();

}  // namespace renderer::sidecar
