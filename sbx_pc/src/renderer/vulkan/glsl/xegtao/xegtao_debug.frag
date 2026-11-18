#version 450
// --xegtao_debug (passes.cpp, DrawXeGTAODebug): one of XeGTAO's buffers
// drawn over the frame, stretched to it.
//   1: the AO, as applied (white: no occlusion)
//   2: the normals made from depth (view space, x red, y green, z blue,
//      shown as normal * 0.5 + 0.5)
//   3: the denoiser's edges (black: an edge on every side)

layout(set = 0, binding = 0) uniform sampler2D ao_history;  // rg32f: .r the visibility
layout(set = 0, binding = 1) uniform usampler2D normals;  // r32ui, 11_11_10
layout(set = 0, binding = 2) uniform sampler2D edges;     // r32f, 2 bits a side

layout(push_constant) uniform Constants {
  vec2 extent;  // the frame's drawn part, in pixels
  int mode;
} constants;

layout(location = 0) out vec4 color_out;

void main() {
  vec2 uv = gl_FragCoord.xy / constants.extent;
  ivec2 size = textureSize(ao_history, 0);
  ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
  vec3 color = vec3(0.0);
  if (constants.mode == 1) {
    color = vec3(texelFetch(ao_history, texel, 0).r);
  } else if (constants.mode == 2) {
    uint packed = texelFetch(normals, texel, 0).r;
    color = vec3(float(packed & 0x7FFu) / 2047.0, float((packed >> 11) & 0x7FFu) / 2047.0,
                 float((packed >> 22) & 0x3FFu) / 1023.0);
  } else if (constants.mode == 3) {
    color = vec3(texelFetch(edges, texel, 0).r);
  }
  color_out = vec4(color, 1.0);
}
