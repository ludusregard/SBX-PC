// The frame dump (the overlay's "Dump this frame" button, render_frame_dump):
// everything one frame drew with, written out after it as frame.json, the
// textures it read from guest memory as DDS files, and the shown image and
// resolves as PFM files (see "Frame dumps" in docs/native-renderer-port.md).
//
// The back end fills a Frame as it draws (raw values only, so recording
// doesn't change the frame's timing) and hands it to Write afterwards with
// a way to read textures. Nothing here is Vulkan's: the values are the
// game's own (Xenos registers, fetch constants, shader hashes).

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace renderer::frame_dump {

// A shader a draw used, and the float constants it read (c0-c255 of its
// stage), each as 4 floats' bits.
struct Shader {
  uint64_t hash = 0;  // MicrocodeHash; 0 for none
  // The hash its port is found by (glsl/game/shaders.json's lookup_hash), 0
  // if it was translated.
  uint64_t port_hash = 0;
  std::vector<uint32_t> registers;
  std::vector<std::array<uint32_t, 4>> values;
};

// A texture a shader samples: the shader's binding, then the fetch
// constant's 6 words (host order).
struct Texture {
  bool pixel = false;  // the pixel shader's, else the vertex shader's
  uint32_t fetch_constant = 0;
  bool is_signed = false;
  bool cube = false;  // declared as a cube (else 2D or 2D array; 3D is "volume")
  bool volume = false;
  std::array<uint32_t, 6> fetch{};
};

struct Draw {
  // The target: the surface's pitch, MSAA, EDRAM base and format, and
  // which of color and depth are bound.
  uint32_t pitch = 0, msaa = 0, edram_base = 0, surface_format = 0;
  bool has_color = false, has_depth = false;
  bool main_view = false;
  bool inline_vertices = false;  // drawn from vertices the library copied in
  uint32_t primitive_type = 0, count = 0;
  Shader vertex, pixel;
  // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, RB_MODECONTROL,
  // RB_COLOR_MASK, RB_BLEND_RED/GREEN/BLUE/ALPHA, RB_ALPHA_REF.
  std::array<uint32_t, 10> render_state{};
  // The 8 bool constant words, then the 32 loop constants.
  std::array<uint32_t, 40> bool_loop_constants{};
  // The vertex fetch constants the vertex shader reads: index, then words.
  std::vector<std::array<uint32_t, 3>> vertex_fetches;
  // Fetch constants 88-95 the vertex shader doesn't read but that hold a
  // vertex buffer (the stream a model's shader may skip, such as the
  // course's normals): index, then words.
  std::vector<std::array<uint32_t, 3>> unread_vertex_fetches;
  std::vector<Texture> textures;
};

// A resolve: the target copied into a texture (and maybe cleared after).
struct Resolve {
  uint32_t after_draws = 0;  // how many of the frame's draws came before it
  uint32_t flags = 0;        // as the XDK's D3DRESOLVE flags
  uint32_t pitch = 0, msaa = 0, edram_base = 0, surface_format = 0;  // the source
  bool from_depth = false;
  bool has_rect = false;
  std::array<int32_t, 4> rect{};
  uint32_t destination = 0;  // the destination texture object (guest address)
  uint32_t destination_address = 0;  // where its data is (physical)
  uint32_t main_view_resolve = 0;  // the main view's nth resolve this frame, 0 if another
};

struct Frame {
  std::vector<Draw> draws;
  std::vector<Resolve> resolves;
};

// A texture read from guest memory for writing out: a DXGI_FORMAT, each
// level's layers in turn, rows of blocks packed.
struct Image {
  uint32_t dxgi_format = 0;
  uint32_t width = 0, height = 0, layers = 1, levels = 1;
  uint32_t first_level = 0;  // 1 when the texture has only mips (the DDS starts there)
  bool cube = false;
  std::vector<std::vector<uint8_t>> subresources;
  // Set (and false returned) when a resolve destination answers for the
  // texture: its memory doesn't hold what's sampled.
  bool resolved = false;
};

// Reads the texture of a fetch constant's words; false if it can't.
using ReadTexture = std::function<bool(const std::array<uint32_t, 6>& fetch, Image& out)>;

// Writes frame.json, and each texture the frame read once as
// textures/<address>_<format>_<width>x<height>.dds (textures that are
// resolve destinations are listed but not written: their memory doesn't
// hold what was resolved into them). Returns how many textures were
// written; errors are logged.
uint32_t Write(const std::filesystem::path& directory, const Frame& frame,
               const ReadTexture& read_texture);

// Writes one image as a DDS file with a DX10 header. False if it can't.
bool WriteDds(const std::filesystem::path& file, const Image& image);

}  // namespace renderer::frame_dump
