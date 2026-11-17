// The pipelines drawn with in earlier sessions, so the next session can make
// them before they're needed (--native_renderer_warm_pipelines). Making a
// pipeline the driver's cache doesn't hold takes 20-100 ms, a stutter the
// first time each shader is drawn after a build changes the shaders or the
// driver changes; made at start on a worker thread instead, the draws find
// them made.
//
// Kept in cache/pipeline_list.bin next to the executable (disk_cache.h),
// appended to as new pipelines appear. Nothing here is Vulkan's: a
// pipeline is its shaders, each by its microcode (kept once a hash, so the
// shaders can be made again from it, as they may be translated or fitted
// differently after a build), how they connect, and the back end's own
// words for the rest of its state.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renderer {

struct PipelineRecord {
  // Each stage's microcode hash (MicrocodeHash; 0: no pixel shader) and
  // register count (from SQ_PROGRAM_CNTL).
  uint64_t vertex_hash = 0, pixel_hash = 0;
  uint32_t vertex_num_reg = 0, pixel_num_reg = 0;
  // ShaderCache::Interface's fields.
  uint32_t interpolator_mask = 0;
  uint32_t param_gen_interpolator = UINT32_MAX;
  bool param_gen_point = false;
  bool rectangle_list = false;
  // The back end's words for the rest (blending, depth, targets...).
  std::vector<uint32_t> state;
};

class PipelineList {
 public:
  ~PipelineList();

  // Reads the file's records and opens it to append more. A file of another
  // version or state size (another back end's, or before a change) is
  // started over.
  void Open(const std::filesystem::path& file, uint32_t state_words);

  const std::vector<PipelineRecord>& records() const { return records_; }

  // A shader's microcode (big-endian words, as in guest memory), or null if
  // the file doesn't hold it.
  const std::vector<uint8_t>* Microcode(uint64_t hash) const;

  // A pipeline made this session: added to the file (with its shaders'
  // microcode, the first time each hash is seen), unless it's already
  // there. The pixel microcode may be null when pixel_hash is 0.
  void Add(const PipelineRecord& record, const uint8_t* vertex_ucode, uint32_t vertex_size,
           const uint8_t* pixel_ucode, uint32_t pixel_size);

 private:
  void Append(uint32_t kind, const std::vector<uint8_t>& payload);

  std::vector<PipelineRecord> records_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> microcode_;
  std::unordered_set<uint64_t> known_;  // the records' hashes
  uint32_t state_words_ = 0;
  std::FILE* file_ = nullptr;
};

}  // namespace renderer
