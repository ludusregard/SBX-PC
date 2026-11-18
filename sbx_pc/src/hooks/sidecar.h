// Sidecar textures: full-size textures in a `.sbpr` beside a pack the patch
// loader serves (src/hooks/sidecar.cpp).

#pragma once

#include <filesystem>
#include <vector>

namespace sidecar {

// Once, from the patch loader (Archive::Init): the files it serves. Each
// one's sidecar, if it has one, is read and its packs indexed.
void IndexSidecars(const std::vector<std::filesystem::path>& files);

}  // namespace sidecar
