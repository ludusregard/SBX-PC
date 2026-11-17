#include "renderer/gpu_memory.h"

#include <format>
#include <iterator>

namespace renderer::gpu_memory {

std::string Describe() {
  static constexpr const char* kKinds[] = {"textures", "resolved", "images",   "staging",
                                           "mirrors",  "constants", "other"};
  static constexpr const char* kObjects[] = {"views", "samplers", "descriptor pools",
                                             "pipelines"};
  static_assert(std::size(kKinds) == size_t(Kind::kCount));
  static_assert(std::size(kObjects) == size_t(Object::kCount));
  Totals& totals = Get();
  int64_t total = 0;
  std::string out;
  for (size_t i = 0; i < size_t(Kind::kCount); i++) {
    int64_t bytes = totals.bytes[i].load(std::memory_order_relaxed);
    total += bytes;
    out += std::format("{} {:.1f} MB ({}); ", kKinds[i], bytes / 1048576.0,
                       totals.allocations[i].load(std::memory_order_relaxed));
  }
  out = std::format("{:.1f} MB: ", total / 1048576.0) + out;
  for (size_t i = 0; i < size_t(Object::kCount); i++) {
    out += std::format("{}{} {}", i ? ", " : "", kObjects[i],
                       totals.objects[i].load(std::memory_order_relaxed));
  }
  return out;
}

}  // namespace renderer::gpu_memory
