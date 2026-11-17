# Part shaders

The shaders in the part files (the cars' and the course objects'
materials), for the mod tool: what each one reads and how the material's
values reach it. Ids are the mod tool's (`sbxr_tools`, `sbx_formats/shaders.py`:
numbered in order of first use over the parts by id). Each is also ported
to GLSL (`docs/shaders.md`), and the port is the clearest way to see
exactly what a shader does: `src/renderer/vulkan/glsl/game/<shader>.frag` or
`.vert`, named in the table below.

Status (2026-11-13): 25 of the 39 were in the part files sampled. Running
`tools/shader_port/port_all.py` over all of `BUILD_OUT` finds the rest and
fills in the table (`write_doc.py parts`).

## How they fit together

A part's shader section holds vertex and pixel shader pairs (`docs/game_data.md`,
"Part files"). Every pair uses the same constant registers for the same
things, so a material's values go to the same place whatever the pair:

Vertex shaders:

| Constant | Registers | What |
|---|---|---|
| `matWVPVS` | c0-c3 | World x view x projection (rows) |
| `matWorldVS` | c4-c7 | World matrix, for the lit shaders' positions and normals |
| `TexOffsetVS` | c9 (c9-c11) | Added to each UV set: one register a set (scrolling textures) |
| `MatColorVS` | c13 | Multiplies the vertex color |

Pixel shaders:

| Constant | Registers | What |
|---|---|---|
| `EyePos` | c4 | The camera, for specular |
| `DLightDir`, `DLightColor`, `DLightSpecularColor` | c5-c7, c41-c43, c44-c46 | Up to 3 directional lights |
| `PntLightPos`, `PntLightAtten`, `PntLightColor`, `PntLightSpecularColor` | c8-c15, c16-c23, c47-c54, c55-c62 | Up to 8 point lights |
| `SpotLightDir`, `SpotLightCone` | c24-c31, c32-c39 | A direction and cone per point light (read with the point lights' index; likely what makes a point light a spot) |
| `AmbientColor` | c40 | Scene ambient, added to the material's |
| `MatParam` | c64 | x: the specular exponent (the specular term is raised to it: log2, times `MatParam.x`, exp2) |
| `MatAmbientDiffuse` | c65 | The material's ambient and diffuse color |
| `MatSpecularColor` | c66 | The material's specular color |
| `ModTexAlpha` | c67 | x scales the first texture's alpha, y the second's (how much the second texture covers the first) |
| `matView` | c0-c2 | The view matrix (only with `spheretx0`: inferred, the sphere map's coordinates) |
| `UseProjTex` | b0 (bool 128) | At the end: when set, the color is kept at 0 or more |

The lights are loops over loop constants, reading the light arrays relative
to the loop index (so, inferred, the loop counts are how many lights are
on). Textures: `tx0`
(fetch constant 0), `tx1` (1), `tx2` (2) are the material's layers;
`spheretx0` (6) a sphere map and `cubetx0` (6) a cube map for reflections.

What the layers do, from the ports (`ps_19` as the example, two textures):
the second texture is blended over the first by its alpha times
`ModTexAlpha.y`; the result's alpha is 1 - (1 - tx0.a x `ModTexAlpha.x`) x
(1 - tx1.a x `ModTexAlpha.y`), times the vertex alpha, saturated; the color
is the lit color (ambient + diffuse from the lights) times the textures,
plus the specular sum. The unlit shaders (`ps_01`, `ps_15`, `ps_25`)
multiply the texture by the vertex color, and `ps_25` is the vertex color
alone.

Which pair a material uses, and how its values are laid out in the part,
is the mod tool's side; this page is what the shaders read.

## The shaders

| Id | Shader | Microcode words | Constants | Textures | Interpolators |
|---|---|---|---|---|---|
| `ps_00` | `ps_9C3EB0925F0E75E8` | 147 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `MatAmbientDiffuse` c65, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `SpotLightCone` c32-39, `SpotLightDir` c24-31 | 0 `tx0` | r0-r7 |
| `ps_01` | `ps_B0F0ACD4B7AFFEC8` | 21 | `ModTexAlpha` c67 | 0 `tx0` | r0-r1 |
| `ps_02` | `ps_76DEE068496E05C1` | 291 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0, `matView` c0-2 | 6 `spheretx0`, 0 `tx0` | r0-r9 |
| `ps_03` | `ps_BDEC67D7B4DF91A1` | 150 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `MatAmbientDiffuse` c65, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `SpotLightCone` c32-39, `SpotLightDir` c24-31 | 0 `tx0` | r0-r7 |
| `ps_04` | `ps_ADC9112A32A92816` | 144 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `MatAmbientDiffuse` c65, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `SpotLightCone` c32-39, `SpotLightDir` c24-31 |  | r0-r6 |
| `ps_05` | `ps_B2B10AE1E0E9FE13` | 276 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0, `matView` c0-2 | 6 `spheretx0` | r0-r8 |
| `ps_06` | `ps_72DE42674BE25160` | 240 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 0 `tx0` | r0-r9 |
| `ps_07` | `ps_503D67F05D6DAA03` | 231 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 |  | r0-r8 |
| `ps_08` | `ps_919CB09ADFA6FA77` | 249 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 6 `cubetx0` (cube) | r0-r8 |
| `ps_14` | `ps_27A79CBF2701BC34` | 294 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 6 `cubetx0` (cube), 0 `tx0`, 1 `tx1` | r0-r10 |
| `ps_15` | `ps_F3C0D4132DB33EFE` | 27 | `ModTexAlpha` c67, `UseProjTex` b0 | 0 `tx0` | r0-r2 |
| `ps_16` | `ps_7ECFFC514C780F01` | 261 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 6 `cubetx0` (cube), 0 `tx0` | r0-r9 |
| `ps_17` | `ps_B69BADFDF194591C` | 261 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 6 `cubetx0` (cube), 2 `tx2` | r0-r9 |
| `ps_19` | `ps_0E1E9AD0349FAC43` | 279 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 0 `tx0`, 1 `tx1` | r0-r10 |
| `ps_21` | `ps_703BAF8642EAEBBC` | 249 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 2 `tx2` | r0-r9 |
| `ps_23` | `ps_BD9BB201950E597D` | 240 | `AmbientColor` c40, `DLightColor` c41-43, `DLightDir` c5-7, `DLightSpecularColor` c44-46, `EyePos` c4, `MatAmbientDiffuse` c65, `MatParam` c64, `MatSpecularColor` c66, `ModTexAlpha` c67, `PntLightAtten` c16-23, `PntLightColor` c47-54, `PntLightPos` c8-15, `PntLightSpecularColor` c55-62, `SpotLightCone` c32-39, `SpotLightDir` c24-31, `UseProjTex` b0 | 1 `tx1` | r0-r9 |
| `ps_25` | `ps_806BEF915614FFDB` | 12 | `UseProjTex` b0 |  | r0-r0 |
| `vs_00` | `vs_C5A6F2B6C0BD3BC4` | 63 | `MatColorVS` c13, `TexOffsetVS` c9, `matWVPVS` c0-3, `matWorldVS` c4-7 |  | o0, o1, o2, o3 |
| `vs_01` | `vs_93F94842A4742732` | 39 | `MatColorVS` c13, `TexOffsetVS` c9, `matWVPVS` c0-3 |  | o0, o1 |
| `vs_02` | `vs_93996C44777D8E9A` | 60 | `MatColorVS` c13, `TexOffsetVS` c9, `matWVPVS` c0-3, `matWorldVS` c4-7 |  | o0, o1, o2, o3 |
| `vs_03` | `vs_07147F7735F408F8` | 54 | `MatColorVS` c13, `matWVPVS` c0-3, `matWorldVS` c4-7 |  | o0, o1, o2 |
| `vs_06` | `vs_C56643D9D12D081E` | 66 | `MatColorVS` c13, `TexOffsetVS` c9-10, `matWVPVS` c0-3, `matWorldVS` c4-7 |  | o0, o1, o2, o3, o4 |
| `vs_07` | `vs_FA17356113DFBFDA` | 36 | `MatColorVS` c13, `TexOffsetVS` c9, `matWVPVS` c0-3 |  | o0, o1 |
| `vs_08` | `vs_9AD90069B3591514` | 72 | `MatColorVS` c13, `TexOffsetVS` c9-11, `matWVPVS` c0-3, `matWorldVS` c4-7 |  | o0, o1, o2, o3, o4, o5 |
| `vs_10` | `vs_BB9F34FE57827C8D` | 30 | `MatColorVS` c13, `matWVPVS` c0-3 |  | o0 |
