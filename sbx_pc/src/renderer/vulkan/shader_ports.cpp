#include "renderer/vulkan/shader_ports.h"

#include <cstring>
#include <unordered_map>

#include "renderer/vulkan/game_shader_ports.h"
#include "renderer/vulkan/port_spirv.h"

namespace renderer {

namespace {

uint32_t Word(const uint8_t* ucode, uint32_t index) {
  const uint8_t* p = ucode + size_t(index) * 4;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

int32_t SignExtend(uint32_t value, int bits) {
  return int32_t(value << (32 - bits)) >> (32 - bits);
}

// The ports' specialization constants (tools/shader_port/ucode_to_glsl.py).
constexpr uint32_t kSpecInterpolatorMask = 0;   // pixel
constexpr uint32_t kSpecParamGenRegister = 1;   // pixel; -1 for none
constexpr uint32_t kSpecRectangleList = 2;      // vertex
constexpr uint32_t kSpecVertexFetches = 100;    // vertex: 10 a fetch, from 100
// The hand-written post effects' sample counts (glsl/post).
constexpr uint32_t kSpecMotionBlurQuality = 10;  // pixel
constexpr uint32_t kSpecBloomQuality = 11;       // pixel

}  // namespace

uint32_t GamePortCount() { return uint32_t(std::size(kGamePorts)); }

uint64_t LayoutFreeHash(const GamePort& port, const uint8_t* ucode, uint32_t size_bytes) {
  std::vector<uint8_t> masked(ucode, ucode + size_bytes);
  auto clear = [&](uint32_t word, uint32_t bits) {
    uint8_t* p = masked.data() + size_t(word) * 4;
    uint32_t v = (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) & ~bits;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  };
  for (uint32_t i = 0; i < port.vertex_fetch_count; i++) {
    uint32_t at = port.vertex_fetches[i] * 3u;
    if ((at + 3) * 4 > size_bytes) {
      return 0;
    }
    clear(at, 0x7Fu << 20);         // fetch constant
    clear(at + 1, 0x3FFFFu << 12);  // format, signedness, exponent
    clear(at + 2, 0xFFFFFFFFu);     // stride, offset
  }
  return MicrocodeHash(masked.data(), size_bytes);
}

const GamePort* FindGamePort(ShaderStage stage, const uint8_t* ucode, uint32_t size_bytes,
                             uint64_t hash) {
  for (const GamePort& port : kGamePorts) {
    if (port.stage != stage || port.microcode_words * 4 != size_bytes) {
      continue;
    }
    if (stage == ShaderStage::kPixel ? port.hash == hash
                                     : port.hash == LayoutFreeHash(port, ucode, size_bytes)) {
      return &port;
    }
  }
  return nullptr;
}

bool MakeFromPort(const GamePort& port, const uint8_t* ucode, uint32_t size_bytes,
                  uint32_t interpolator_mask, uint32_t param_gen_interpolator,
                  bool rectangle_list, TranslatedShader& shader,
                  const PostQuality& post_quality) {
  PortSpecialization specialization;
  specialization.vertex = port.stage == ShaderStage::kVertex;
  specialization.interpolator_mask = interpolator_mask & 0xFFFF;
  auto& constants = specialization.constants;
  if (specialization.vertex) {
    constants.push_back({kSpecRectangleList, rectangle_list ? 1u : 0u});
    // Each vertex fetch's fields as the game patched them.
    for (uint32_t i = 0; i < port.vertex_fetch_count; i++) {
      uint32_t at = port.vertex_fetches[i] * 3u;
      if ((at + 3) * 4 > size_bytes) {
        return false;
      }
      uint32_t w0 = Word(ucode, at), w1 = Word(ucode, at + 1), w2 = Word(ucode, at + 2);
      uint32_t fetch_constant = ((w0 >> 20) & 31) * 3 + ((w0 >> 25) & 3);
      uint32_t flags = ((w1 >> 12) & 1)          // signed
                       | ((w1 >> 13) & 1) << 1   // integer
                       | ((w1 >> 14) & 1) << 2   // signed values have no zero
                       | ((w1 >> 15) & 1) << 3;  // the index is rounded
      uint32_t base = kSpecVertexFetches + 10 * i;
      constants.push_back({base + 0, fetch_constant});
      constants.push_back({base + 1, uint32_t(SignExtend((w2 >> 8) & 0x7FFFFF, 23))});
      constants.push_back({base + 2, w2 & 0xFF});
      constants.push_back({base + 3, (w1 >> 16) & 63});
      constants.push_back({base + 4, flags});
      constants.push_back({base + 5, uint32_t(SignExtend((w1 >> 24) & 63, 6))});
    }
  } else {
    constants.push_back({kSpecInterpolatorMask, specialization.interpolator_mask});
    constants.push_back({kSpecParamGenRegister,
                         param_gen_interpolator < 16 ? param_gen_interpolator : 0xFFFFFFFFu});
    constants.push_back({kSpecMotionBlurQuality, post_quality.motion_blur});
    constants.push_back({kSpecBloomQuality, post_quality.bloom});
  }
  shader.spirv = SpecializePort(port.spirv, port.spirv_words, specialization);
  if (shader.spirv.empty()) {
    return false;
  }
  shader.textures.clear();
  for (uint32_t i = 0; i < port.texture_count; i++) {
    const GamePortTexture& t = port.textures[i];
    shader.textures.push_back({t.fetch_constant,
                               t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                               t.is_signed});
  }
  shader.samplers.clear();
  for (uint32_t i = 0; i < port.sampler_count; i++) {
    const GamePortSampler& s = port.samplers[i];
    shader.samplers.push_back(
        {s.fetch_constant, s.mag_filter, s.min_filter, s.mip_filter, s.aniso_filter});
  }
  return true;
}

}  // namespace renderer
