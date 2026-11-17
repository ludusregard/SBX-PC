// The game's shaders ported to GLSL (src/renderer/vulkan/glsl/game, made by
// tools/shader_port; docs/shaders.md): looking a shader up by its microcode,
// and fitting the port to a draw.
//
// A port gives the same values as the SDK's translation of the same
// microcode, with the same descriptors (tools/shader_port/check compares
// them), so ShaderCache can hand it out in the translation's place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "renderer/vulkan/shaders.h"

namespace renderer {

struct GamePortTexture {
  uint32_t fetch_constant;
  bool cube;  // else a 2D array (1D and 2D textures)
  bool is_signed;
};

struct GamePortSampler {
  uint32_t fetch_constant;
  uint32_t mag_filter, min_filter, mip_filter, aniso_filter;
};

struct GamePort {
  ShaderStage stage;
  // Pixel shaders: MicrocodeHash of the microcode. Vertex shaders: the same
  // with each vertex fetch's layout fields zeroed (LayoutFreeHash), as the
  // game patches those for the vertex declaration in use.
  uint64_t hash;
  uint32_t microcode_words;
  const uint32_t* spirv;
  size_t spirv_words;
  const char* found_in;  // where tools/shader_port found it first
  // The vertex fetch instructions, in order (instruction index: words
  // 3n to 3n+2 of the microcode).
  std::array<uint16_t, 16> vertex_fetches;
  uint32_t vertex_fetch_count;
  // The descriptors, in the translator's order.
  std::array<GamePortTexture, 32> textures;
  uint32_t texture_count;
  std::array<GamePortSampler, 16> samplers;
  uint32_t sampler_count;
};

// The port of this microcode (as in guest memory, big-endian words), or
// null. hash: its MicrocodeHash.
const GamePort* FindGamePort(ShaderStage stage, const uint8_t* ucode, uint32_t size_bytes,
                             uint64_t hash);

// The microcode's hash with the port's vertex fetches' layout fields zeroed.
uint64_t LayoutFreeHash(const GamePort& port, const uint8_t* ucode, uint32_t size_bytes);

// The port's SPIR-V for a draw: the vertex fetches' fields from the (patched)
// microcode, the interpolators' locations, and the rest of the interface.
// Its textures and samplers go in `shader`. False if it can't be made.
// How many samples the hand-written post effects take, as multiples of the
// game's (1 is the game's): the motion blur's taps along the motion, the
// bloom's Gaussian blur taps per texel. A shader without the setting
// ignores it.
struct PostQuality {
  uint32_t motion_blur = 1;
  uint32_t bloom = 1;
};

bool MakeFromPort(const GamePort& port, const uint8_t* ucode, uint32_t size_bytes,
                  uint32_t interpolator_mask, uint32_t param_gen_interpolator,
                  bool rectangle_list, TranslatedShader& shader,
                  const PostQuality& post_quality = {});

uint32_t GamePortCount();

}  // namespace renderer
