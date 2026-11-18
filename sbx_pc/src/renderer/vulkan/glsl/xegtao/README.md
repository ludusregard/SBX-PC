# XeGTAO

`XeGTAO.h` and `XeGTAO.hlsli` are Intel's XeGTAO ambient occlusion, version
1.30, unmodified, from https://github.com/GameTechDev/XeGTAO (commit
a5b1686c7ea37788eeb3576b5be47f7c03db532c; Copyright (C) 2016-2021, Intel
Corporation; MIT License, see `LICENSE.txt`). XeGTAO is based on
"Practical Real-Time Strategies for Accurate Indirect Occlusion" (Jimenez et
al.).

`sbx_xegtao.hlsl` is ours: the sample's entry points (its `vaGTAO.hlsl`)
with Vulkan bindings. `xegtao_depth.comp` (the AO's own depth, without the
glass), `xegtao_normals.comp`, `xegtao_temporal.comp`,
`xegtao_apply.frag` and `xegtao_debug.frag` are ours too: the materials'
smooth normals merged over the ones from depth, the AO kept over frames, the
AO multiplied into the 3D view, and `--xegtao_debug`'s views.

`../compile_xegtao.py` compiles them all (patching `XeGTAO.hlsli` as text
for glslang, and adding `--xegtao_plane_bias`'s test to the main pass;
listed at the top of that script) into `../../generated_xegtao.h`, used by
`--xegtao` (`ApplyXeGTAO` in `../../passes.cpp`). `passes.cpp` also
includes `XeGTAO.h` itself, for `XeGTAO::GTAOUpdateConstants`.
