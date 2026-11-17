#include "renderer/frame_dump.h"

#include "base/log.h"
#include "renderer/disk_cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>

namespace renderer::frame_dump {
namespace {

std::string Hex(uint64_t value) {
  return std::format("\"{:016X}\"", value);
}

std::string Hex32(uint32_t value) {
  return std::format("\"{:08X}\"", value);
}

float AsFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// A float for JSON (which has no infinities or NaNs: those as strings).
std::string Number(float value) {
  if (value != value) {
    return "\"nan\"";
  }
  if (value > 3.4e38f || value < -3.4e38f) {
    return value > 0 ? "\"inf\"" : "\"-inf\"";
  }
  return std::format("{}", value);
}

template <typename T, typename F>
std::string List(const T& items, F&& format) {
  std::string out = "[";
  bool first = true;
  for (const auto& item : items) {
    out += first ? "" : ", ";
    out += format(item);
    first = false;
  }
  return out + "]";
}

// The texture fetch constant's fields (as the SDK's xe_gpu_texture_fetch_t).
struct FetchFields {
  uint32_t format, address, mip_address, width, height, depth, dimension, swizzle, endian;
  bool tiled, packed_mips;
  uint32_t mip_min, mip_max;
  int32_t exp_adjust;
};

FetchFields Fields(const std::array<uint32_t, 6>& f) {
  FetchFields out{};
  out.format = f[1] & 0x3F;
  out.endian = (f[1] >> 6) & 3;
  out.tiled = (f[0] >> 31) & 1;
  out.address = f[1] & 0xFFFFF000u;
  out.dimension = (f[5] >> 9) & 3;  // 0 1D, 1 2D or stacked, 2 3D, 3 cube
  out.width = (f[2] & 0x1FFF) + 1;
  out.height = ((f[2] >> 13) & 0x1FFF) + 1;
  out.depth = ((f[2] >> 26) & 0x3F) + 1;
  out.swizzle = (f[3] >> 1) & 0xFFF;          // bits 1-12: 3 bits a component
  out.exp_adjust = int32_t(f[3] << 13) >> 26;  // bits 13-18, signed
  out.packed_mips = (f[5] >> 11) & 1;
  out.mip_address = f[5] & 0xFFFFF000u;
  out.mip_min = (f[4] >> 2) & 0xF;
  out.mip_max = (f[4] >> 6) & 0xF;
  return out;
}

const char* kDimensions[] = {"1d", "2d", "3d", "cube"};

std::string ShaderJson(const Shader& shader) {
  if (!shader.hash) {
    return "null";
  }
  std::string constants = "{";
  for (size_t i = 0; i < shader.registers.size() && i < shader.values.size(); i++) {
    const auto& v = shader.values[i];
    constants += std::format("{}\"c{}\": [{}, {}, {}, {}]", i ? ", " : "", shader.registers[i],
                             Number(AsFloat(v[0])), Number(AsFloat(v[1])),
                             Number(AsFloat(v[2])), Number(AsFloat(v[3])));
  }
  constants += "}";
  return std::format("{{\"hash\": {}, \"port\": {}, \"float_constants\": {}}}",
                     Hex(shader.hash), shader.port_hash ? Hex(shader.port_hash) : "null",
                     constants);
}

std::string TextureFileName(const FetchFields& f) {
  return std::format("{:08X}_format{}_{}x{}.dds", f.address ? f.address : f.mip_address,
                     f.format, f.width, f.height);
}

}  // namespace

bool WriteDds(const std::filesystem::path& file, const Image& image) {
  if (!image.dxgi_format || image.subresources.size() != size_t(image.levels) * image.layers) {
    return false;
  }
  // DDS_HEADER and DDS_HEADER_DXT10 (DirectXTex's DDS.h), little-endian.
  uint32_t header[31] = {};
  header[0] = 124;                                   // size
  header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;    // caps, height, width, pixel format, mips
  header[2] = std::max(image.height >> image.first_level, 1u);
  header[3] = std::max(image.width >> image.first_level, 1u);
  header[6] = image.levels;
  header[18] = 32;                                   // the pixel format's size
  header[19] = 0x4;                                  // a FourCC
  std::memcpy(&header[20], "DX10", 4);
  header[26] = 0x1000 | (image.levels > 1 ? 0x400008 : 0);  // texture, mipmap, complex
  if (image.cube) {
    header[26] |= 0x8;
    header[27] = 0x200 | 0xFC00;                     // a cube map with all 6 faces
  }
  uint32_t dx10[5] = {image.dxgi_format, 3 /* 2D */, image.cube ? 0x4u : 0u,
                      image.cube ? image.layers / 6 : image.layers, 0};
  std::FILE* out = OpenFile(file, "wb");
  if (!out) {
    return false;
  }
  bool ok = std::fwrite("DDS ", 1, 4, out) == 4 && std::fwrite(header, 4, 31, out) == 31 &&
            std::fwrite(dx10, 4, 5, out) == 5;
  // DDS order: each layer's levels in turn (ours is each level's layers).
  for (uint32_t layer = 0; ok && layer < image.layers; layer++) {
    for (uint32_t level = 0; ok && level < image.levels; level++) {
      const std::vector<uint8_t>& data = image.subresources[size_t(level) * image.layers + layer];
      ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    }
  }
  ok = std::fclose(out) == 0 && ok;
  return ok;
}

uint32_t Write(const std::filesystem::path& directory, const Frame& frame,
               const ReadTexture& read_texture) {
  std::error_code error;
  std::filesystem::create_directories(directory / "textures", error);
  // Each texture once, by its file name (address, format and size: the
  // sampler fields of the words don't change what's read): its file, and
  // whether a resolve destination answers for it.
  struct Written {
    std::string file;
    bool resolved = false;
  };
  std::map<std::string, Written> texture_files;
  std::set<uint32_t> resolved;  // destination addresses resolved into this frame
  for (const Resolve& r : frame.resolves) {
    resolved.insert(r.destination_address & 0xFFFFF000u);
  }
  uint32_t written = 0;
  std::string json = "{\n  \"draws\": [\n";
  for (size_t n = 0; n < frame.draws.size(); n++) {
    const Draw& d = frame.draws[n];
    std::string textures = "[";
    for (size_t i = 0; i < d.textures.size(); i++) {
      const Texture& t = d.textures[i];
      FetchFields f = Fields(t.fetch);
      std::string name = TextureFileName(f);
      auto [found, added] = texture_files.try_emplace(name);
      Written& w = found->second;
      if (added) {
        Image image;
        w.resolved = resolved.count(f.address) != 0;
        if (!w.resolved && read_texture(t.fetch, image)) {
          if (WriteDds(directory / "textures" / name, image)) {
            w.file = "textures/" + name;
            written++;
          } else {
            ITC_LOG_WARN("native renderer: frame dump: couldn't write texture {}", name);
          }
        }
        w.resolved = w.resolved || image.resolved;
      }
      textures += std::format(
          "{}\n        {{\"stage\": \"{}\", \"fetch_constant\": {}, \"signed\": {}, "
          "\"declared\": \"{}\", \"address\": {}, \"mip_address\": {}, \"format\": {}, "
          "\"width\": {}, \"height\": {}, \"depth\": {}, \"dimension\": \"{}\", \"tiled\": {}, "
          "\"packed_mips\": {}, \"endian\": {}, \"swizzle\": \"{:04o}\", \"exp_adjust\": {}, "
          "\"resolve_destination\": {}, \"file\": {}, \"words\": {}}}",
          i ? "," : "", t.pixel ? "ps" : "vs", t.fetch_constant, t.is_signed,
          t.cube ? "cube" : (t.volume ? "3d" : "2d"), Hex32(f.address), Hex32(f.mip_address),
          f.format, f.width, f.height, f.depth, kDimensions[f.dimension & 3], f.tiled,
          f.packed_mips, f.endian, f.swizzle, f.exp_adjust,
          w.resolved, w.file.empty() ? "null" : "\"" + w.file + "\"",
          List(t.fetch, [](uint32_t w) { return Hex32(w); }));
    }
    textures += d.textures.empty() ? "]" : "\n      ]";
    const auto& rs = d.render_state;
    json += std::format(
        "    {{\"draw\": {}, \"main_view\": {}, \"target\": {{\"pitch\": {}, \"msaa\": {}, "
        "\"edram_base\": {}, \"format\": {}, \"color\": {}, \"depth\": {}}}, \"primitive\": {}, "
        "\"count\": {}, \"inline_vertices\": {},\n"
        "      \"vs\": {},\n      \"ps\": {},\n"
        "      \"render_state\": {{\"depthcontrol\": {}, \"blendcontrol\": {}, "
        "\"colorcontrol\": {}, \"modecontrol\": {}, \"color_mask\": {}, \"blend_constant\": "
        "[{}, {}, {}, {}], \"alpha_ref\": {}}},\n"
        "      \"bool_constants\": {}, \"loop_constants\": {},\n"
        "      \"vertex_fetches\": {},\n"
        "      \"textures\": {}}}{}\n",
        n, d.main_view, d.pitch, d.msaa, d.edram_base, d.surface_format, d.has_color,
        d.has_depth, d.primitive_type, d.count, d.inline_vertices, ShaderJson(d.vertex),
        ShaderJson(d.pixel), Hex32(rs[0]), Hex32(rs[1]), Hex32(rs[2]), Hex32(rs[3]),
        Hex32(rs[4]), Number(AsFloat(rs[5])), Number(AsFloat(rs[6])), Number(AsFloat(rs[7])),
        Number(AsFloat(rs[8])), Number(AsFloat(rs[9])),
        List(std::vector<uint32_t>(d.bool_loop_constants.begin(),
                                   d.bool_loop_constants.begin() + 8),
             [](uint32_t w) { return Hex32(w); }),
        List(std::vector<uint32_t>(d.bool_loop_constants.begin() + 8,
                                   d.bool_loop_constants.end()),
             [](uint32_t w) { return Hex32(w); }),
        List(d.vertex_fetches,
             [](const std::array<uint32_t, 3>& v) {
               return std::format("{{\"fetch_constant\": {}, \"address\": {}, \"bytes\": {}, "
                                  "\"words\": [{}, {}]}}",
                                  v[0], Hex32(v[1] & 0x1FFFFFFCu), ((v[2] >> 2) & 0xFFFFFFu) * 4,
                                  Hex32(v[1]), Hex32(v[2]));
             }),
        textures, n + 1 < frame.draws.size() ? "," : "");
  }
  json += "  ],\n  \"resolves\": [\n";
  for (size_t n = 0; n < frame.resolves.size(); n++) {
    const Resolve& r = frame.resolves[n];
    json += std::format(
        "    {{\"after_draws\": {}, \"flags\": {}, \"from_depth\": {}, \"source\": {{\"pitch\": "
        "{}, \"msaa\": {}, \"edram_base\": {}, \"format\": {}}}, \"rect\": {}, "
        "\"destination\": {}, \"destination_address\": {}, \"main_view_resolve\": {}}}{}\n",
        r.after_draws, Hex32(r.flags), r.from_depth, r.pitch, r.msaa, r.edram_base,
        r.surface_format,
        r.has_rect ? std::format("[{}, {}, {}, {}]", r.rect[0], r.rect[1], r.rect[2], r.rect[3])
                   : std::string("null"),
        Hex32(r.destination), Hex32(r.destination_address), r.main_view_resolve,
        n + 1 < frame.resolves.size() ? "," : "");
  }
  json += "  ]\n}\n";
  std::filesystem::path path = directory / "frame.json";
  std::FILE* out = OpenFile(path, "wb");
  bool ok = out && std::fwrite(json.data(), 1, json.size(), out) == json.size();
  if (out) {
    ok = std::fclose(out) == 0 && ok;
  }
  if (!ok) {
    ITC_LOG_ERROR("native renderer: frame dump: couldn't write {}", path.string());
  }
  ITC_LOG_INFO("native renderer: frame dump: {} draws, {} resolves and {} textures in {}",
               frame.draws.size(), frame.resolves.size(), written, directory.string());
  return written;
}

}  // namespace renderer::frame_dump
