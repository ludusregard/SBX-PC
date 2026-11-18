// Sidecar textures (docs/game_data.md, "Sidecar textures (SBPR)"): a pack
// served from PATCH_DATA or a language folder can have full-size versions of
// its textures in `<same name>.sbpr` beside it (sbxr_tools writes both). The
// pack holds a stand-in for each: a texture of the size and format the game
// expects (a smaller copy, or its header alone and a blank page). The game
// loads and uses the stand-ins; the renderer draws the full-size textures in
// their place.
//
// The patch loader hands over the files it serves (IndexSidecars). For each
// with a sidecar, the XPR2s the sidecar names (by their offset in the file:
// 0 for an .xpr, further in for a screen's or a font's) are read from the
// file, and the bytes of each one's header (the 12-byte head and the
// header block: records, texture headers, names) are hashed.
//
// XPR::Create (sub_8257C880) gets the XPR2 as loaded. Its wrapper hashes the
// same bytes; for a pack with a sidecar, once the original has copied the
// data to physical memory and relocated the textures, it reads each listed
// texture's relocated fetch constant and registers its address with the
// renderer (src/renderer/sidecar_textures.h). The texture cache then uploads
// the full-size DDS for a texture at that address of the stand-in's format
// and size, while the memory there still holds the stand-in's data.

#include "hooks/sidecar.h"

#include "guest.h"
#include "renderer/sidecar_textures.h"

#include "base/hash.h"

#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kXpr2Magic = 0x58505232;  // "XPR2"
constexpr uint32_t kSbprEntrySize = 64;
// The fetch constant's place in a texture header.
constexpr uint32_t kTextureFetch = 0x1C;
// XPR::Create's object.
constexpr uint32_t kXprHeader = 0, kXprRecords = 16, kXprRecordCount = 20;

// A sidecar's entry: a texture of one XPR2 in the file.
struct Listed {
  uint32_t index;
  std::string name;
  uint32_t full_width, full_height, stand_width, stand_height;
  uint64_t dds_offset, dds_size;
  uint32_t levels;
};

// An XPR2 with full-size textures: its sidecar and those textures.
struct Pack {
  std::filesystem::path sidecar;
  std::string label;  // the file and offset, for the log
  std::vector<Listed> textures;
};

// By the hash of the XPR2's header bytes. Filled once (patch loader), read
// only after.
std::unordered_map<uint64_t, Pack> g_packs;

// Stand-ins registered so far (page and name), to log each once.
std::mutex g_logged_mutex;
std::set<std::pair<uint32_t, std::string>> g_logged;

uint32_t LoadLE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// A guest address's physical page, as the GPU (and the texture cache) sees
// it: the 0xA0000000 and 0xC0000000 views map physical memory as is, the
// 0xE0000000 view 4 KiB further in (as Xenia maps them). XPR::Create
// relocates textures to the data's virtual address (0xFC8E8000 for 22591's
// first logo), which the GPU reads as physical 0x1C8E9000.
uint32_t PhysicalPage(uint32_t address) {
  uint32_t physical = address & 0x1FFFFFFF;
  if (address >= 0xE0000000) {
    physical += 0x1000;
  }
  return (physical & 0x1FFFFFFF) >> 12;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// The sidecar beside a file: same name, extension .sbpr (any case).
std::filesystem::path SidecarOf(const std::filesystem::path& file) {
  std::string stem = file.stem().string();
  std::error_code ec;
  std::filesystem::directory_iterator it(file.parent_path(), ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path& p = it->path();
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".sbpr" && p.stem().string() == stem && it->is_regular_file(ec)) {
      return p;
    }
  }
  return {};
}

void IndexOne(const std::filesystem::path& file, const std::filesystem::path& sidecar) {
  const std::string shown = file.filename().string();
  std::vector<uint8_t> s = ReadFile(sidecar);
  if (s.size() < 16 || std::memcmp(s.data(), "SBPR", 4) != 0 || LoadLE32(&s[4]) != 1) {
    REXLOG_WARN("sidecar: {} isn't an SBPR sidecar (version 1); ignored",
                sidecar.filename().string());
    return;
  }
  uint32_t count = LoadLE32(&s[8]);
  if (16 + uint64_t(count) * kSbprEntrySize > s.size()) {
    REXLOG_WARN("sidecar: {}: its {} entries run past its end; ignored",
                sidecar.filename().string(), count);
    return;
  }
  std::vector<uint8_t> f = ReadFile(file);
  // The entries by the XPR2 they're for.
  std::map<uint32_t, std::vector<Listed>> by_xpr;
  for (uint32_t k = 0; k < count; k++) {
    const uint8_t* e = &s[16 + size_t(k) * kSbprEntrySize];
    Listed t;
    uint32_t xpr = LoadLE32(e);
    t.index = LoadLE32(e + 4);
    const char* name = reinterpret_cast<const char*>(e + 8);
    t.name.assign(name, strnlen(name, 32));
    t.full_width = LoadLE32(e + 40);
    t.full_height = LoadLE32(e + 44);
    t.stand_width = LoadLE32(e + 48);
    t.stand_height = LoadLE32(e + 52);
    t.dds_offset = LoadLE32(e + 56);
    t.dds_size = LoadLE32(e + 60);
    uint32_t width = 0, height = 0;
    std::string error;
    if (t.dds_offset + t.dds_size > s.size() ||
        !renderer::sidecar::ReadHeader(sidecar, t.dds_offset, t.dds_size, width, height,
                                       t.levels, error)) {
      REXLOG_WARN("sidecar: {}: {} (texture {}): {}; skipped", sidecar.filename().string(),
                  t.name, t.index, error.empty() ? "its DDS runs past the end" : error);
      continue;
    }
    if (width != t.full_width || height != t.full_height) {
      REXLOG_WARN("sidecar: {}: {}: the entry says {}x{}, its DDS {}x{}; the DDS's size is "
                  "used",
                  sidecar.filename().string(), t.name, t.full_width, t.full_height, width,
                  height);
      t.full_width = width;
      t.full_height = height;
    }
    by_xpr[xpr].push_back(std::move(t));
  }
  for (auto& [offset, textures] : by_xpr) {
    if (uint64_t(offset) + 16 > f.size() || LoadBE32(&f[offset]) != kXpr2Magic) {
      REXLOG_WARN("sidecar: {}: no XPR2 at offset {} in {}; its {} textures skipped",
                  sidecar.filename().string(), offset, shown, textures.size());
      continue;
    }
    uint64_t head = 12 + uint64_t(LoadBE32(&f[offset + 4]));
    if (offset + head > f.size()) {
      REXLOG_WARN("sidecar: {}: the XPR2 at {} in {} is cut short; skipped",
                  sidecar.filename().string(), offset, shown);
      continue;
    }
    uint64_t hash = base::Hash64(&f[offset], size_t(head));
    Pack& pack = g_packs[hash];
    if (!pack.textures.empty()) {
      REXLOG_WARN("sidecar: the XPR2 at {} in {} has the same header as {}; {}'s textures "
                  "are used",
                  offset, shown, pack.label, pack.label);
      continue;
    }
    pack.sidecar = sidecar;
    pack.label = offset ? shown + " +" + std::to_string(offset) : shown;
    pack.textures = std::move(textures);
    REXLOG_INFO("sidecar: {}: {} full-size textures for {}", sidecar.filename().string(),
                pack.textures.size(), pack.label);
  }
}

}  // namespace

namespace sidecar {

void IndexSidecars(const std::vector<std::filesystem::path>& files) {
  for (const auto& file : files) {
    std::filesystem::path s = SidecarOf(file);
    if (!s.empty()) {
      IndexOne(file, s);
    }
  }
}

}  // namespace sidecar

// XPR::Create: r3 the XPR object, r4 the XPR2 as loaded.
extern "C" REX_FUNC(sub_8257C880) {
  REX_FUNC_PROLOGUE();
  const uint32_t object = ctx.r3.u32;
  const uint32_t file = ctx.r4.u32;
  const Pack* pack = nullptr;
  if (!g_packs.empty() && file && guest::LoadU32(base, file) == kXpr2Magic) {
    uint64_t head = 12 + uint64_t(guest::LoadU32(base, file + 4));
    auto it = g_packs.find(base::Hash64(base + file, size_t(head)));
    if (it != g_packs.end()) {
      pack = &it->second;
    }
  }
  __imp__sub_8257C880(ctx, base);
  if (!pack) {
    return;
  }

  const uint32_t header = guest::LoadU32(base, object + kXprHeader);
  const uint32_t records = guest::LoadU32(base, object + kXprRecords);
  const uint32_t count = guest::LoadU32(base, object + kXprRecordCount);
  if (!header || !records) {
    REXLOG_WARN("sidecar: XPR::Create failed for {}; its stand-ins are drawn", pack->label);
    return;
  }
  const uint32_t header_size = guest::LoadU32(base, file + 4);
  size_t registered = 0;
  for (const Listed& t : pack->textures) {
    if (t.index >= count) {
      REXLOG_WARN("sidecar: {}: texture {} ({}) isn't in the pack ({} records)", pack->label,
                  t.index, t.name, count);
      continue;
    }
    const uint32_t record = records + 16 * t.index;
    // The record's offset is from the header block's start (it may have
    // been made a pointer, as the name's is).
    uint32_t offset = guest::LoadU32(base, record + 4);
    uint32_t texture = offset < header_size ? header + offset : offset;
    uint32_t name_at = guest::LoadU32(base, record + 12);
    std::string name;
    for (uint32_t i = 0; i < 32 && name_at; i++) {
      char c = char(guest::LoadU8(base, name_at + i));
      if (!c) {
        break;
      }
      name.push_back(c);
    }
    if (name != t.name) {
      REXLOG_WARN("sidecar: {}: texture {} is {}, the sidecar says {}; skipped", pack->label,
                  t.index, name, t.name);
      continue;
    }
    uint32_t fetch[6];
    for (uint32_t i = 0; i < 6; i++) {
      fetch[i] = guest::LoadU32(base, texture + kTextureFetch + 4 * i);
    }
    renderer::sidecar::Entry entry;
    if (!renderer::sidecar::StandIn(fetch, entry.page, entry.guest_format, entry.width,
                                    entry.height)) {
      REXLOG_WARN("sidecar: {}: {}: not a 2D texture with a base level; skipped", pack->label,
                  t.name);
      continue;
    }
    if (entry.width != t.stand_width || entry.height != t.stand_height) {
      REXLOG_WARN("sidecar: {}: {} is {}x{} in the pack, the sidecar's stand-in {}x{}; "
                  "skipped",
                  pack->label, t.name, entry.width, entry.height, t.stand_width,
                  t.stand_height);
      continue;
    }
    // The header holds the data's virtual address: the GPU's page is the
    // physical one.
    entry.page = PhysicalPage(fetch[1] & 0xFFFFF000);
    entry.hash_size = 4096;
    entry.data_hash = renderer::sidecar::HashGuest(base, entry.page, entry.hash_size);
    entry.file = pack->sidecar;
    entry.offset = t.dds_offset;
    entry.size = t.dds_size;
    entry.full_width = t.full_width;
    entry.full_height = t.full_height;
    entry.levels = t.levels;
    entry.name = t.name;
    uint32_t page = entry.page, format = entry.guest_format;
    renderer::sidecar::Register(std::move(entry));
    registered++;
    std::lock_guard lock(g_logged_mutex);
    if (g_logged.insert({page, t.name}).second) {
      REXLOG_INFO("sidecar: {}: {} ({}x{} format {} stand-in at {:08X}, fetch {:08X} {:08X} "
                  "{:08X}) drawn at {}x{}",
                  pack->label, t.name, t.stand_width, t.stand_height, format, page << 12,
                  fetch[0], fetch[1], fetch[2], t.full_width, t.full_height);
    }
  }
  if (registered != pack->textures.size()) {
    REXLOG_WARN("sidecar: {}: {} of {} full-size textures registered", pack->label, registered,
                pack->textures.size());
  }
}
