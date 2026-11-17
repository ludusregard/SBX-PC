// The game's unlit material: the vertex color, times a color texture if
// there is one.
//
// variants.txt picks the features for each of the game's shaders:
//   COLOR_REG, UV0_REG  the interpolators: the vertex color and the
//                       texture coordinates
//   TX0_UV              color texture tx0 and its coordinates
#include "common.glsl"

layout(location = COLOR_REG) in vec4 v_color;
#ifdef UV0_REG
layout(location = UV0_REG) in vec4 uv0;
#endif

layout(location = 0) out vec4 out_color;

void main() {
  vec4 color = v_color;
#ifdef TX0_UV
  vec4 texel = sample_2d(tx0, TX0_UV.xy);
  color.rgb *= texel.rgb;
  color.a *= texel.a * ModTexAlpha.x;
#endif
  color.a = clamp(color.a, 0.0, 1.0);

  if (!xe_alpha_test_passes(color.a)) discard;
  out_color = xe_color_output(color, 0);
}
