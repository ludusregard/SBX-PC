// Sidecar textures: the stand-ins registered, and their DDS files read
// (src/renderer/sidecar_textures.h).

#include "renderer/sidecar_textures.h"

#include "base/flags.h"
#include "base/hash.h"
#include "base/log.h"
#include "renderer/state.h"

#include <rex/graphics/xenos.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

ITC_FLAG_BOOL(sidecar_textures, true, "SBX",
              "Draw the full-size textures in a pack's .sbpr sidecar (PATCH_DATA, the "
              "language folders) in the place of their stand-ins; off draws the stand-ins");

namespace renderer::sidecar {

namespace xenos = rex::graphics::xenos;

namespace {

std::mutex g_mutex;
std::map<uint32_t, std::shared_ptr<const Entry>> g_entries;  // by page
// Images read, by (file, offset), while a texture holds them.
std::map<std::pair<std::filesystem::path, uint64_t>, std::weak_ptr<const Image>> g_images;
// Entries whose sidecar couldn't be read: logged once each.
std::set<std::pair<std::filesystem::path, uint64_t>> g_failed;
std::atomic<uint64_t> g_generation{0};
// Pages whose stand-in didn't match a fetch constant (logged once each).
std::set<uint32_t> g_mismatch_logged;
// Pages registered since the texture cache last asked (TakeRegistered).
std::vector<uint32_t> g_new_pages;
// Pages matched to a registration elsewhere (logged once each).
std::set<uint32_t> g_moved_logged;

// How far from its registered page a stand-in is looked for. 0: only there
// (the page is the physical one: see sidecar.cpp, PhysicalPage). Kept as a
// knob in case a texture turns out to be drawn from a copy nearby.
constexpr uint32_t kNearPages = 0;
std::atomic<bool> g_last_enabled{true};

uint32_t LoadLE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // the host is little-endian
}

// A DDS header's format: the Vulkan format and its blocks. Data starts at
// `data_start`.
bool ParseHeader(const uint8_t* h, size_t size, Image& out, size_t& data_start,
                 std::string& error) {
  if (size < 128 || std::memcmp(h, "DDS ", 4) != 0 || LoadLE32(h + 4) != 124) {
    error = "not a DDS";
    return false;
  }
  out.height = LoadLE32(h + 12);
  out.width = LoadLE32(h + 16);
  out.levels = std::max(LoadLE32(h + 28), 1u);
  uint32_t pf_flags = LoadLE32(h + 80);
  uint32_t four_cc = LoadLE32(h + 84);
  data_start = 128;
  auto block = [&](VkFormat format, uint32_t bytes) {
    out.format = format;
    out.block_width = out.block_height = 4;
    out.bytes_per_block = bytes;
  };
  auto plain = [&](VkFormat format, uint32_t bytes) {
    out.format = format;
    out.block_width = out.block_height = 1;
    out.bytes_per_block = bytes;
  };
  if (pf_flags & 0x4) {  // a FourCC
    if (four_cc == LoadLE32(reinterpret_cast<const uint8_t*>("DXT1"))) {
      block(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
    } else if (four_cc == LoadLE32(reinterpret_cast<const uint8_t*>("DXT3"))) {
      block(VK_FORMAT_BC2_UNORM_BLOCK, 16);
    } else if (four_cc == LoadLE32(reinterpret_cast<const uint8_t*>("DXT5"))) {
      block(VK_FORMAT_BC3_UNORM_BLOCK, 16);
    } else if (four_cc == LoadLE32(reinterpret_cast<const uint8_t*>("DX10"))) {
      if (size < 148) {
        error = "DX10 header cut short";
        return false;
      }
      data_start = 148;
      switch (LoadLE32(h + 128)) {  // DXGI_FORMAT
        case 71:  // BC1_UNORM
          block(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
          break;
        case 74:  // BC2_UNORM
          block(VK_FORMAT_BC2_UNORM_BLOCK, 16);
          break;
        case 77:  // BC3_UNORM
          block(VK_FORMAT_BC3_UNORM_BLOCK, 16);
          break;
        case 98:  // BC7_UNORM
        case 99:  // BC7_UNORM_SRGB, read as stored like the game's textures
          block(VK_FORMAT_BC7_UNORM_BLOCK, 16);
          break;
        case 28:  // R8G8B8A8_UNORM
          plain(VK_FORMAT_R8G8B8A8_UNORM, 4);
          break;
        case 87:  // B8G8R8A8_UNORM
          plain(VK_FORMAT_B8G8R8A8_UNORM, 4);
          break;
        case 11:  // R16G16B16A16_UNORM
          plain(VK_FORMAT_R16G16B16A16_UNORM, 8);
          break;
        case 10:  // R16G16B16A16_FLOAT
          plain(VK_FORMAT_R16G16B16A16_SFLOAT, 8);
          break;
        default:
          error = "DXGI format " + std::to_string(LoadLE32(h + 128)) + " isn't read";
          return false;
      }
    } else {
      error = "FourCC isn't read";
      return false;
    }
  } else if ((pf_flags & 0x40) && LoadLE32(h + 88) == 32) {  // 32-bit RGB(A)
    uint32_t red = LoadLE32(h + 92);
    if (red == 0x000000FF) {
      plain(VK_FORMAT_R8G8B8A8_UNORM, 4);
    } else if (red == 0x00FF0000) {
      plain(VK_FORMAT_B8G8R8A8_UNORM, 4);
    } else {
      error = "32-bit masks aren't read";
      return false;
    }
  } else {
    error = "pixel format isn't read";
    return false;
  }
  if (!out.width || !out.height || out.width > 16384 || out.height > 16384) {
    error = "size out of range";
    return false;
  }
  out.levels = std::min(out.levels, 32 - uint32_t(std::countl_zero(std::max(out.width,
                                                                              out.height))));
  return true;
}

void LayOutLevels(Image& image) {
  size_t at = 0;
  image.level_offsets.clear();
  image.level_sizes.clear();
  for (uint32_t level = 0; level < image.levels; level++) {
    uint32_t w = std::max(image.width >> level, 1u);
    uint32_t h = std::max(image.height >> level, 1u);
    size_t size = size_t((w + image.block_width - 1) / image.block_width) *
                  ((h + image.block_height - 1) / image.block_height) * image.bytes_per_block;
    image.level_offsets.push_back(at);
    image.level_sizes.push_back(size);
    at += size;
  }
}

}  // namespace

bool Enabled() {
  bool enabled = ITC_FLAG(sidecar_textures);
  if (g_last_enabled.exchange(enabled, std::memory_order_relaxed) != enabled) {
    g_generation.fetch_add(1, std::memory_order_relaxed);
  }
  return enabled;
}

bool ReadHeader(const std::filesystem::path& file, uint64_t offset, uint64_t size,
                uint32_t& width, uint32_t& height, uint32_t& levels, std::string& error) {
  uint8_t header[148] = {};
  std::ifstream in(file, std::ios::binary);
  in.seekg(std::streamoff(offset));
  in.read(reinterpret_cast<char*>(header), std::streamsize(std::min<uint64_t>(size, 148)));
  if (!in && in.gcount() < 128) {
    error = "can't read it";
    return false;
  }
  Image image;
  size_t data_start = 0;
  if (!ParseHeader(header, size_t(std::min<uint64_t>(size, 148)), image, data_start, error)) {
    return false;
  }
  LayOutLevels(image);
  if (data_start + image.level_offsets.back() + image.level_sizes.back() > size) {
    error = "its levels run past its size";
    return false;
  }
  width = image.width;
  height = image.height;
  levels = image.levels;
  return true;
}

bool StandIn(const uint32_t fetch_words[6], uint32_t& page, uint32_t& format, uint32_t& width,
             uint32_t& height) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_words, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked) {
    return false;
  }
  page = fetch.base_address & 0x1FFFF;
  format = uint32_t(fetch.format);
  width = fetch.size_2d.width + 1u;
  height = fetch.size_2d.height + 1u;
  return page != 0;
}

uint64_t HashData(const uint8_t* data, uint32_t size) {
  return base::Hash64(data, size);
}

uint64_t HashGuest(uint8_t* base, uint32_t page, uint32_t size) {
  return base::Hash64(PhysicalHostAddress(base, page << 12), size);
}

void Register(Entry entry) {
  uint32_t page = entry.page;
  auto shared = std::make_shared<const Entry>(std::move(entry));
  {
    std::lock_guard lock(g_mutex);
    g_entries[page] = std::move(shared);
    g_new_pages.push_back(page);
  }
  g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const Entry> Match(uint8_t* base, const uint32_t fetch[6]) {
  if (!Enabled()) {
    return nullptr;
  }
  uint32_t page, format, width, height;
  if (!StandIn(fetch, page, format, width, height)) {
    return nullptr;
  }
  std::lock_guard lock(g_mutex);
  if (g_entries.empty()) {
    return nullptr;
  }
  // The stand-in of this format and size at the address (or the nearest
  // within kNearPages) whose data, with guest memory given, is there.
  std::shared_ptr<const Entry> best;
  uint32_t best_distance = UINT32_MAX;
  auto from = g_entries.lower_bound(page > kNearPages ? page - kNearPages : 0);
  for (auto it = from; it != g_entries.end() && it->first <= page + kNearPages; ++it) {
    const Entry& e = *it->second;
    if (e.guest_format != format || e.width != width || e.height != height) {
      continue;
    }
    uint32_t distance = it->first > page ? it->first - page : page - it->first;
    if (distance >= best_distance) {
      continue;
    }
    if (base && HashGuest(base, page, e.hash_size) != e.data_hash) {
      continue;
    }
    best = it->second;
    best_distance = distance;
  }
  if (best && best_distance && base && g_moved_logged.insert(page).second) {
    ITC_LOG_INFO("sidecar: {}: drawn from {:08X}, {} pages from where it was registered "
                 "({:08X})",
                 best->name, page << 12, int64_t(page) - int64_t(best->page), best->page << 12);
  }
  if (!best && base && !g_mismatch_logged.count(page)) {
    for (auto it = from; it != g_entries.end() && it->first <= page + kNearPages; ++it) {
      const Entry& e = *it->second;
      if (e.guest_format == format && e.width == width && e.height == height) {
        g_mismatch_logged.insert(page);
        ITC_LOG_WARN("sidecar: a texture at {:08X} (format {} {}x{}) is where {}'s stand-in "
                     "was ({:08X}) but its data isn't the stand-in's; drawn from the guest",
                     page << 12, format, width, height, e.name, e.page << 12);
        break;
      }
    }
  }
  return best;
}

std::shared_ptr<const Image> Load(const Entry& entry) {
  auto key = std::make_pair(entry.file, entry.offset);
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_images.find(key); it != g_images.end()) {
      if (auto image = it->second.lock()) {
        return image;
      }
    }
    if (g_failed.count(key)) {
      return nullptr;
    }
  }
  auto image = std::make_shared<Image>();
  std::string error;
  std::vector<uint8_t> bytes(size_t(entry.size));
  std::ifstream in(entry.file, std::ios::binary);
  in.seekg(std::streamoff(entry.offset));
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  size_t data_start = 0;
  if (!in) {
    error = "can't read it";
  } else if (ParseHeader(bytes.data(), bytes.size(), *image, data_start, error)) {
    LayOutLevels(*image);
    size_t end = data_start + image->level_offsets.back() + image->level_sizes.back();
    if (end > bytes.size()) {
      error = "its levels run past its size";
    } else {
      image->data.assign(bytes.begin() + ptrdiff_t(data_start), bytes.begin() + ptrdiff_t(end));
    }
  }
  std::lock_guard lock(g_mutex);
  if (image->data.empty()) {
    g_failed.insert(key);
    ITC_LOG_WARN("sidecar: {} in {} (at {}): {}; the stand-in is drawn", entry.name,
                 entry.file.string(), entry.offset, error);
    return nullptr;
  }
  ITC_LOG_INFO("sidecar: {} read from {}: {}x{}, {} levels, {:.1f} MB (stand-in {}x{})",
               entry.name, entry.file.filename().string(), image->width, image->height,
               image->levels, image->data.size() / 1048576.0, entry.width, entry.height);
  g_images[key] = image;
  return image;
}

std::vector<uint32_t> TakeRegistered() {
  std::lock_guard lock(g_mutex);
  return std::exchange(g_new_pages, {});
}

uint64_t Generation() {
  Enabled();  // a change of the flag counts
  return g_generation.load(std::memory_order_relaxed);
}

}  // namespace renderer::sidecar
