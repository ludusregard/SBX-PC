#pragma once

#include "guest.h"

namespace renderer {

// The library's DrawIndexedVertices (sub_825A3ED0), without its GPU packets
// in exclusive mode; see draw_packets.cpp.
void DrawPackets_IndexedDraw(PPCContext& ctx, uint8_t* base);

}  // namespace renderer
