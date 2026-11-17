"""Lists a frame dump's draws with the game's names for what they use.

A frame dump (the overlay's "Dump this frame" button, see "Frame dumps" in
docs/native-renderer-port.md) is a folder with frame.json, textures/*.dds
and *.pfm images. This reads frame.json and names, from the repository's
shader notes:

- each shader: the hand-written file and defines it's made from
  (src/renderer/vulkan/glsl/*/variants.txt) and where it was found
  (glsl/game/shaders.json);
- each float and bool constant by the game's name for it (matView,
  DLightColor, ...), with its values;
- each texture by the sampler name the shader gives it (tx0, spheretx0,
  cubetx0, ...), with its size, format and DDS file.

Usage:
  py describe.py <dump folder> [--draw N] [--ps HASH] [--vs HASH] [--main-view]
                 [--resolves] [--out FILE]

HASH is a shader hash as frame.json and variants.txt write it (the port's
hash for vertex shaders). Without filters every draw is listed.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GLSL = os.path.join(HERE, "..", "..", "src", "renderer", "vulkan", "glsl")
VARIANT_FOLDERS = ["materials", "post", "boot", "vertex"]

# Xenos texture formats by number (the ones this game uses, and neighbors).
TEXTURE_FORMATS = {
    2: "8", 3: "1_5_5_5", 4: "5_6_5", 6: "8_8_8_8", 7: "2_10_10_10", 10: "8_8",
    15: "4_4_4_4", 18: "DXT1", 19: "DXT2_3", 20: "DXT4_5", 22: "24_8", 23: "24_8_FLOAT",
    24: "16", 25: "16_16", 26: "16_16_16_16", 30: "16_FLOAT", 31: "16_16_FLOAT",
    32: "16_16_16_16_FLOAT", 36: "32_FLOAT", 37: "32_32_FLOAT", 38: "32_32_32_32_FLOAT",
    49: "DXN", 50: "8_8_8_8_AS_16_16_16_16", 51: "DXT1_AS_16_16_16_16",
    52: "DXT2_3_AS_16_16_16_16", 53: "DXT4_5_AS_16_16_16_16",
    54: "2_10_10_10_AS_16_16_16_16",
}


def load_variants():
    """Shader name (ps_HASH / vs_HASH) -> (file, defines)."""
    variants = {}
    for folder in VARIANT_FOLDERS:
        path = os.path.join(GLSL, folder, "variants.txt")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    variants[parts[0]] = (folder + "/" + parts[1], " ".join(parts[2:]))
    return variants


def load_shaders():
    """Stage and hash ("ps", "0E7B...") -> shaders.json's entry, by its name's
    hash and by the hash the renderer finds its port by (lookup_hash: for
    vertex shaders, the microcode's with the vertex fetch layouts zeroed, as
    frame.json's "port" writes it)."""
    path = os.path.join(GLSL, "game", "shaders.json")
    if not os.path.exists(path):
        return {}
    shaders = {}
    with open(path, encoding="utf-8") as f:
        for s in json.load(f):
            shaders[(s["stage"], s["hash"].upper())] = s
            shaders[(s["stage"], s.get("lookup_hash", s["hash"]).upper())] = s
    return shaders


def entry_of(stage, shader, shaders):
    """shaders.json's entry for a dumped shader, or None."""
    if not shader:
        return None
    return shaders.get((stage, (shader.get("port") or shader["hash"]).upper()))


def constant_names(entry, kind):
    """Register -> name for one set (float4, bool, sampler) of a shader."""
    names = {}
    for c in (entry or {}).get("constants", []):
        if c.get("set") != kind:
            continue
        count = c.get("count", 1)
        for i in range(count):
            register = c["register"] + i
            name = c["name"] if count == 1 else "%s[%d]" % (c["name"], i)
            names.setdefault(register, []).append(name)
    return {r: "/".join(n) for r, n in names.items()}


def shader_lines(stage, shader, shaders, variants):
    if not shader:
        return ["  %s: none" % stage]
    entry = entry_of(stage, shader, shaders)
    name = "%s_%s" % (stage, entry["hash"] if entry else shader["hash"])
    lines = []
    source = variants.get(name)
    where = ", ".join(entry.get("found_in", [])) if entry else ""
    head = "  %s %s" % (stage, shader["hash"])
    if entry and entry["hash"] != shader["hash"]:
        head += " (%s)" % name
    if source:
        head += ": %s %s" % source
    elif shader.get("port"):
        head += ": glsl/game/%s" % name
    else:
        head += ": translated"
    if where:
        head += "  [found in %s]" % where
    lines.append(head)
    names = constant_names(entry, "float4")
    for register, values in sorted(shader["float_constants"].items(),
                                   key=lambda item: int(item[0][1:])):
        number = int(register[1:])
        lines.append("    %-5s %-24s %s" % (
            register, names.get(number, ""),
            " ".join("%10.5g" % v if isinstance(v, (int, float)) else "%10s" % v
                     for v in values)))
    return lines


def describe(dump, args, out):
    with open(os.path.join(dump, "frame.json"), encoding="utf-8") as f:
        frame = json.load(f)
    variants = load_variants()
    shaders = load_shaders()
    resolves = frame.get("resolves", [])
    next_resolve = 0
    for draw in frame["draws"]:
        n = draw["draw"]
        while args.resolves and next_resolve < len(resolves) and \
                resolves[next_resolve]["after_draws"] <= n:
            print_resolve(next_resolve, resolves[next_resolve], out)
            next_resolve += 1
        vs, ps = draw.get("vs"), draw.get("ps")
        if args.draw is not None and n != args.draw:
            continue
        if args.main_view and not draw["main_view"]:
            continue
        if args.ps and not (ps and args.ps.upper() in (ps["hash"], ps.get("port") or "")):
            continue
        if args.vs and not (vs and args.vs.upper() in (vs["hash"], vs.get("port") or "")):
            continue
        t = draw["target"]
        out.write("draw %d: %s, target pitch %d MSAA %d format %d%s%s, primitive %d x%d%s\n" % (
            n, "main view" if draw["main_view"] else "other", t["pitch"], t["msaa"], t["format"],
            ", color" if t["color"] else "", ", depth" if t["depth"] else "",
            draw["primitive"], draw["count"], " (inline)" if draw["inline_vertices"] else ""))
        for line in shader_lines("vs", vs, shaders, variants):
            out.write(line + "\n")
        for line in shader_lines("ps", ps, shaders, variants):
            out.write(line + "\n")
        # Bool constants by name: the vertex shader's are bits 0-127, the
        # pixel shader's 128-255 (as the ports read them, xe_bool(128 + n)).
        bool_names = {}
        for stage, shader, first in (("vs", vs, 0), ("ps", ps, 128)):
            for r, name in constant_names(entry_of(stage, shader, shaders), "bool").items():
                bool_names[first + r] = name
        words = [int(w, 16) for w in draw["bool_constants"]]
        named = ["%s=%d" % (name, (words[r // 32] >> (r % 32)) & 1)
                 for r, name in sorted(bool_names.items())]
        if named:
            out.write("  bools: %s\n" % ", ".join(named))
        rs = draw["render_state"]
        out.write("  depthcontrol %s blendcontrol %s colorcontrol %s color mask %s\n" % (
            rs["depthcontrol"], rs["blendcontrol"], rs["colorcontrol"], rs["color_mask"]))
        for texture in draw["textures"]:
            if texture["signed"]:
                continue  # the same texture's signed view
            stage = texture["stage"]
            shader = vs if stage == "vs" else ps
            sampler = constant_names(entry_of(stage, shader, shaders), "sampler").get(
                texture["fetch_constant"], "")
            out.write("  %s tex %d %-12s %s %dx%d %s (%s), swizzle %s: %s\n" % (
                stage, texture["fetch_constant"], sampler, texture["address"], texture["width"],
                texture["height"], texture["dimension"],
                TEXTURE_FORMATS.get(texture["format"], "format %d" % texture["format"]),
                texture["swizzle"],
                texture["file"] or ("a resolve destination" if texture["resolve_destination"]
                                    else "not written (format not read)")))
        out.write("\n")
    while args.resolves and next_resolve < len(resolves):
        print_resolve(next_resolve, resolves[next_resolve], out)
        next_resolve += 1


def print_resolve(n, r, out):
    s = r["source"]
    out.write("resolve %d (after %d draws): %s of pitch %d format %d into %s (data at %s)%s\n\n" % (
        n, r["after_draws"], "depth" if r["from_depth"] else "color", s["pitch"], s["format"],
        r["destination"], r["destination_address"],
        ", the main view's %d" % r["main_view_resolve"] if r["main_view_resolve"] else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dump", help="the dump's folder (with frame.json)")
    parser.add_argument("--draw", type=int, help="only this draw")
    parser.add_argument("--ps", help="only draws of this pixel shader")
    parser.add_argument("--vs", help="only draws of this vertex shader")
    parser.add_argument("--main-view", action="store_true", help="only the 3D view's draws")
    parser.add_argument("--resolves", action="store_true", help="list the resolves in order too")
    parser.add_argument("--out", help="write here instead of the console")
    args = parser.parse_args()
    if args.out:
        with open(args.out, "w", encoding="utf-8") as out:
            describe(args.dump, args, out)
    else:
        describe(args.dump, args, sys.stdout)


if __name__ == "__main__":
    main()
