#include "renderer/pipeline_list.h"

#include "base/hash.h"
#include "base/log.h"
#include "renderer/disk_cache.h"

#include <algorithm>
#include <cstring>

namespace renderer {
namespace {

constexpr uint32_t kMagic = 0x4C504253;  // "SBPL"
constexpr uint32_t kVersion = 1;
enum Kind : uint32_t { kMicrocode = 1, kPipeline = 2 };

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t bytes[4];
  std::memcpy(bytes, &value, 4);
  out.insert(out.end(), bytes, bytes + 4);
}

void PutU64(std::vector<uint8_t>& out, uint64_t value) {
  PutU32(out, uint32_t(value));
  PutU32(out, uint32_t(value >> 32));
}

// Reads from a record's bytes; ok() turns false past the end.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
  uint32_t U32() {
    uint32_t value = 0;
    if (end_ - p_ < 4) {
      ok_ = false;
      return 0;
    }
    std::memcpy(&value, p_, 4);
    p_ += 4;
    return value;
  }
  uint64_t U64() {
    uint64_t low = U32();
    return low | uint64_t(U32()) << 32;
  }
  const uint8_t* rest() const { return p_; }
  size_t left() const { return size_t(end_ - p_); }
  bool ok() const { return ok_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  bool ok_ = true;
};

std::vector<uint8_t> PipelinePayload(const PipelineRecord& r) {
  std::vector<uint8_t> out;
  PutU64(out, r.vertex_hash);
  PutU32(out, r.vertex_num_reg);
  PutU64(out, r.pixel_hash);
  PutU32(out, r.pixel_num_reg);
  PutU32(out, r.interpolator_mask);
  PutU32(out, r.param_gen_interpolator);
  PutU32(out, r.param_gen_point);
  PutU32(out, r.rectangle_list);
  for (uint32_t word : r.state) {
    PutU32(out, word);
  }
  return out;
}

}  // namespace

PipelineList::~PipelineList() {
  if (file_) {
    std::fclose(file_);
  }
}

void PipelineList::Open(const std::filesystem::path& file, uint32_t state_words) {
  state_words_ = state_words;
  std::vector<uint8_t> data;
  if (std::FILE* in = OpenFile(file, "rb")) {
    uint8_t buffer[65536];
    while (size_t n = std::fread(buffer, 1, sizeof(buffer), in)) {
      data.insert(data.end(), buffer, buffer + n);
    }
    std::fclose(in);
  }
  std::vector<uint8_t> header;
  PutU32(header, kMagic);
  PutU32(header, kVersion);
  PutU32(header, state_words);
  // The records up to the first that's cut short or unknown (a write
  // interrupted by the game closing): the file is cut back to there.
  size_t keep = 0;
  if (data.size() >= header.size() && std::equal(header.begin(), header.end(), data.begin())) {
    keep = header.size();
    while (data.size() - keep >= 8) {
      uint32_t kind, size;
      std::memcpy(&kind, &data[keep], 4);
      std::memcpy(&size, &data[keep + 4], 4);
      if (size > data.size() - keep - 8) {
        break;
      }
      Reader r(&data[keep + 8], size);
      if (kind == kMicrocode) {
        uint64_t hash = r.U64();
        if (!r.ok()) {
          break;
        }
        microcode_[hash].assign(r.rest(), r.rest() + r.left());
      } else if (kind == kPipeline) {
        PipelineRecord record;
        record.vertex_hash = r.U64();
        record.vertex_num_reg = r.U32();
        record.pixel_hash = r.U64();
        record.pixel_num_reg = r.U32();
        record.interpolator_mask = r.U32();
        record.param_gen_interpolator = r.U32();
        record.param_gen_point = r.U32() != 0;
        record.rectangle_list = r.U32() != 0;
        for (uint32_t i = 0; i < state_words; i++) {
          record.state.push_back(r.U32());
        }
        if (!r.ok() || r.left()) {
          break;
        }
        std::vector<uint8_t> payload = PipelinePayload(record);
        if (known_.insert(base::Hash64(payload.data(), payload.size())).second) {
          records_.push_back(std::move(record));
        }
      } else {
        break;
      }
      keep += 8 + size;
    }
  }
  std::error_code error;
  if (keep == 0) {
    file_ = OpenFile(file, "wb");
    if (file_ && (std::fwrite(header.data(), header.size(), 1, file_) != 1 ||
                  std::fflush(file_) != 0)) {
      std::fclose(file_);
      file_ = nullptr;
    }
  } else {
    if (keep < data.size()) {
      std::filesystem::resize_file(file, keep, error);
    }
    file_ = error ? nullptr : OpenFile(file, "ab");
  }
  if (!file_) {
    ITC_LOG_WARN("native renderer: pipeline list: can't write {}", file.string());
  }
  ITC_LOG_INFO("native renderer: pipeline list {}: {} pipelines, {} shaders{}", file.string(),
               records_.size(), microcode_.size(),
               keep == 0 && !data.empty() ? " (old file started over)" : "");
}

const std::vector<uint8_t>* PipelineList::Microcode(uint64_t hash) const {
  auto it = microcode_.find(hash);
  return it == microcode_.end() ? nullptr : &it->second;
}

void PipelineList::Add(const PipelineRecord& record, const uint8_t* vertex_ucode,
                       uint32_t vertex_size, const uint8_t* pixel_ucode, uint32_t pixel_size) {
  if (record.state.size() != state_words_) {
    return;
  }
  std::vector<uint8_t> payload = PipelinePayload(record);
  if (!known_.insert(base::Hash64(payload.data(), payload.size())).second) {
    return;
  }
  records_.push_back(record);
  auto add_microcode = [&](uint64_t hash, const uint8_t* ucode, uint32_t size) {
    if (!hash || !ucode || microcode_.count(hash)) {
      return;
    }
    std::vector<uint8_t>& kept = microcode_[hash];
    kept.assign(ucode, ucode + size);
    std::vector<uint8_t> out;
    PutU64(out, hash);
    out.insert(out.end(), kept.begin(), kept.end());
    Append(kMicrocode, out);
  };
  add_microcode(record.vertex_hash, vertex_ucode, vertex_size);
  add_microcode(record.pixel_hash, pixel_ucode, pixel_size);
  Append(kPipeline, payload);
}

void PipelineList::Append(uint32_t kind, const std::vector<uint8_t>& payload) {
  if (!file_) {
    return;
  }
  uint32_t head[2] = {kind, uint32_t(payload.size())};
  if (std::fwrite(head, sizeof(head), 1, file_) != 1 ||
      std::fwrite(payload.data(), payload.size(), 1, file_) != 1 || std::fflush(file_) != 0) {
    ITC_LOG_WARN("native renderer: pipeline list: write failed; not writing more");
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace renderer
