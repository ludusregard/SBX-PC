#version 450
// --xegtao (passes.cpp, ApplyXeGTAO): the AO into every sample of the 3D
// view, but not into the light the light target holds (what the draws that
// only add light added: glow planes, emissive lights, flares; "View
// targets"): color * ao + light * (1 - ao). The pipeline blends source plus
// destination times source alpha (alpha kept), so this outputs light *
// (1 - ao) with ao as its alpha. MULTISAMPLE: the light target's samples
// averaged.
//   binding 0: the AO history (rg32f: .r the visibility; xegtao_temporal.comp)
//   binding 1: the light target (rgba16f), or a black texel without one

layout(set = 0, binding = 0) uniform sampler2D ao_history;
#ifdef MULTISAMPLE
layout(set = 0, binding = 1) uniform sampler2DMS light;
#else
layout(set = 0, binding = 1) uniform sampler2D light;
#endif

layout(push_constant) uniform Constants {
  float strength;  // 0: no change; 1: the AO as XeGTAO gives it
} constants;

layout(location = 0) out vec4 color_out;

void main() {
  ivec2 position = ivec2(gl_FragCoord.xy);
  float visibility = texelFetch(ao_history, position, 0).r;
  float ao = mix(1.0, clamp(visibility, 0.0, 1.0), constants.strength);
#ifdef MULTISAMPLE
  int samples = textureSamples(light);
  vec3 added = vec3(0.0);
  for (int i = 0; i < samples; i++) {
    added += texelFetch(light, position, i).rgb;
  }
  added /= float(samples);
#else
  ivec2 size = textureSize(light, 0);
  vec3 added = texelFetch(light, min(position, size - 1), 0).rgb;
#endif
  color_out = vec4(max(added, vec3(0.0)) * (1.0 - ao), ao);
}
