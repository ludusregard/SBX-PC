// The data tables as our own C++ (--native_data_tables, on by default):
// their set-up and loads at boot, the loads' callbacks (the entries swapped
// in place, SwapWord included) and the lookups. The archive's loads stay
// recompiled.
//
// The swaps: the originals swap each word through a float register (lfs,
// stfs to the stack, the bytes stored reversed). Here the bytes are
// swapped, as the 360 does it: the recompiled round trip through a double
// sets the quiet bit of a word that reads as a signaling NaN.

#include "base/settings.h"
#include "game/archive.h"
#include "game/car_spec.h"
#include "game/data_tables.h"

SBX_SETTING_BOOL(native_data_tables, true, "Overrides", "Data and Saves",
                 "Our own C++ for the data tables (the item records and the tables loaded "
                 "with them at boot: their loads, byte swaps and lookups) instead of the "
                 "recompiled functions; the archive's loads stay recompiled");

namespace {

using game::AssetSize;
using game::DataEntry20;
using game::DataEntry40;
using game::DataEntry56;
using game::DataTables;
using game::ItemRecord;
using game::SwapBytes;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::u32_be;

constexpr uint32_t kTables = 0x826B5980;    // [it] the table object (writable)
constexpr uint32_t kTableIds = 0x82606BEC;  // the eight 56-byte tables' asset ids (writable)

// The loads' callbacks (DataTables::On...Loaded, below).
constexpr uint32_t kOnItemsLoaded = 0x82117240;
constexpr uint32_t kOnLoaded84 = 0x82117228;
constexpr uint32_t kOn56Loaded = 0x82117258;
constexpr uint32_t kOn20Loaded = 0x82117158;
constexpr uint32_t kOn40Loaded = 0x821171C0;

constexpr uint32_t kItemsAsset = 94;
constexpr uint32_t kAsset84 = 84;
constexpr uint32_t kAsset95 = 95;
constexpr uint32_t kAsset96 = 96;

DataTables* Tables() { return At<DataTables>(*At<u32_be>(kTables)); }

uint32_t TableId(int k) { return At<u32_be>(kTableIds)[k]; }

// The entries' swaps: every word. (The originals leave the last word's
// address in r3; only this file's callbacks call them.)
void Swap(DataEntry20& entry) {
  SwapBytes(entry.key_0);
  SwapBytes(entry.unknown_4);
  SwapBytes(entry.key_8);
  SwapBytes(entry.unknown_12);
}

void Swap(DataEntry40& entry) {
  SwapBytes(entry.unknown_0);
  SwapBytes(entry.end);
  SwapBytes(entry.unknown_8);
}

void Swap(DataEntry56& entry) {
  SwapBytes(entry.first);
  SwapBytes(entry.unknown_4);
}

// A table's entries swapped up to and including the one that ends it
// (`IsEnd` once swapped), then the loads pending counted down.
template <typename Entry, typename IsEnd>
void SwapTable(uint32_t table, IsEnd is_end) {
  for (uint32_t i = 0;; i++) {
    Entry* entry = Element<Entry>(table, i);
    Swap(*entry);
    if (is_end(*entry)) {
      break;
    }
  }
  Tables()->loads_pending -= 1;
}

uint32_t Toc(Guest& g, uint32_t id) { return g.Call(sub_8209D988, id); }

// Archive::Load (sub_8209DF80): asset `id` into `to`, `callback` called
// with r6 = `context` once it's loaded; its serial.
uint32_t Load(Guest& g, uint32_t id, uint32_t to, uint32_t callback, uint32_t context) {
  return g.Call(sub_8209DF80, id, to, callback, context, 0u, 0u);
}

}  // namespace

// DataTables::Init: the object made (operator new) into [0x826B5980], then
// two passes over the twelve assets: the first works out the sizes (each
// table 0, there being no block yet), then the block (operator new[] of the
// total), and the second sets each table's address in it. The loads
// pending are counted up from 0 for each table, each pass.
void game::InitDataTables(Guest& g) {
  const uint32_t object = g.Call(sub_8258C000, uint32_t(sizeof(DataTables)));  // operator new
  *At<u32_be>(kTables) = object;
  At<DataTables>(object)->block = 0;
  Tables()->loads_pending = 0;
  Tables()->item_count = 0;
  if (Tables()->block != 0) {
    return;
  }
  // The table `size` bytes into the block, or 0 while there's no block
  // (`at` the table before it, 0 then too).
  auto after = [](uint32_t at, uint32_t size) { return at != 0 ? Tables()->block + size : 0; };
  uint32_t total = 0;
  for (;;) {
    if (total != 0) {
      Tables()->block = g.Call(sub_8258C0C0, total);  // operator new[]
    }
    uint32_t table = Tables()->block;
    Tables()->loads_pending = 0;
    Tables()->loads_pending += 1;
    Tables()->items = table;
    uint32_t size = AssetSize(Toc(g, kItemsAsset), kItemsAsset);
    table = after(table, size);

    Tables()->loads_pending += 1;
    Tables()->table_84 = table;
    size += AssetSize(Toc(g, kAsset84), kAsset84);
    table = after(table, size);

    for (int k = 0; k < 8; k++) {
      Tables()->loads_pending += 1;
      Tables()->tables_56[k] = table;
      size += g.Call(sub_820A0860, TableId(k));  // Archive::AssetSize
      table = after(table, size);
    }

    Tables()->loads_pending += 1;
    Tables()->table_20 = table;
    size += AssetSize(Toc(g, kAsset95), kAsset95);
    table = after(table, size);

    Tables()->loads_pending += 1;
    Tables()->table_40 = table;
    total = AssetSize(Toc(g, kAsset96), kAsset96) + size;
    if (Tables()->block != 0) {
      break;
    }
  }
}

// DataTables::Load: the twelve loads into the tables Init set, each serial
// kept.
void game::LoadDataTables(Guest& g) {
  DataTables* tables = Tables();
  Tables()->serial = Load(g, kItemsAsset, tables->items, kOnItemsLoaded, AddressOf(tables));
  tables = Tables();
  Tables()->serial = Load(g, kAsset84, tables->table_84, kOnLoaded84, AddressOf(tables));
  for (int k = 0; k < 8; k++) {
    const uint32_t table = Tables()->tables_56[k];
    Tables()->serial = Load(g, TableId(k), table, kOn56Loaded, table);
  }
  uint32_t table = Tables()->table_20;
  Tables()->serial = Load(g, kAsset95, table, kOn20Loaded, table);
  table = Tables()->table_40;
  Tables()->serial = Load(g, kAsset96, table, kOn40Loaded, table);
}

#define SBX_DATA_TABLES_FALLBACK(name)     \
  REX_FUNC_PROLOGUE();                     \
  if (!SBX_GET(native_data_tables)) {  \
    __imp__##name(ctx, base);              \
    return;                                \
  }                                        \
  Guest g(ctx, base);

// SwapWord: r3 = a word, its four bytes reversed in place (the entries'
// swaps' step; the plain C++ of those swaps does it inline).
extern "C" REX_FUNC(sub_820C9578) {
  SBX_DATA_TABLES_FALLBACK(sub_820C9578)
  SwapBytes(*At<u32_be>(ctx.r3.u32));
}

// --- The set-up and the loads (once, at boot) ---

// DataTables::Init: r3 not read.
extern "C" REX_FUNC(sub_82116F18) {
  SBX_DATA_TABLES_FALLBACK(sub_82116F18)
  game::InitDataTables(g);
}

// DataTables::Load: r3 not read.
extern "C" REX_FUNC(sub_821173A8) {
  SBX_DATA_TABLES_FALLBACK(sub_821173A8)
  game::LoadDataTables(g);
}

// --- The loads' callbacks (r6 = the context) ---

// The item records: the item count, and the loads pending.
extern "C" REX_FUNC(sub_82117240) {
  SBX_DATA_TABLES_FALLBACK(sub_82117240)
  DataTables* tables = At<DataTables>(ctx.r6.u32);
  tables->item_count = DataTables::kItemCount;
  tables->loads_pending -= 1;
}

// Asset 84: nothing to swap.
extern "C" REX_FUNC(sub_82117228) {
  SBX_DATA_TABLES_FALLBACK(sub_82117228)
  Tables()->loads_pending -= 1;
}

// The eight 56-byte tables, asset 95's 20-byte table and asset 96's 40-byte
// table, each up to the entry that ends it.
extern "C" REX_FUNC(sub_82117258) {
  SBX_DATA_TABLES_FALLBACK(sub_82117258)
  SwapTable<DataEntry56>(ctx.r6.u32, [](const DataEntry56& e) { return e.first == -1; });
}

extern "C" REX_FUNC(sub_82117158) {
  SBX_DATA_TABLES_FALLBACK(sub_82117158)
  SwapTable<DataEntry20>(ctx.r6.u32, [](const DataEntry20& e) { return e.key_0 == -1; });
}

extern "C" REX_FUNC(sub_821171C0) {
  SBX_DATA_TABLES_FALLBACK(sub_821171C0)
  SwapTable<DataEntry40>(ctx.r6.u32, [](const DataEntry40& e) { return e.end == -1; });
}

// The entries' swaps on their own (r3 the entry).
extern "C" REX_FUNC(sub_82111A70) {
  SBX_DATA_TABLES_FALLBACK(sub_82111A70)
  Swap(*At<DataEntry20>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_82111AC0) {
  SBX_DATA_TABLES_FALLBACK(sub_82111AC0)
  Swap(*At<DataEntry56>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_82112A30) {
  SBX_DATA_TABLES_FALLBACK(sub_82112A30)
  Swap(*At<DataEntry40>(ctx.r3.u32));
}

// --- The lookups (no bounds checks, as the originals) ---

// DataTables::IsLoading: 1 while loads are pending.
extern "C" REX_FUNC(sub_821172C0) {
  SBX_DATA_TABLES_FALLBACK(sub_821172C0)
  ctx.r3.u64 = Tables()->loads_pending != 0 ? 1 : 0;
}

// DataTables::Item: r3 the item; its record.
extern "C" REX_FUNC(sub_821172E0) {
  SBX_DATA_TABLES_FALLBACK(sub_821172E0)
  ctx.r3.u64 = AddressOf(Element<ItemRecord>(Tables()->items, ctx.r3.u32));
}

// ItemRecords::Item: r4 the item (r3 not read).
extern "C" REX_FUNC(sub_82116F10) {
  SBX_DATA_TABLES_FALLBACK(sub_82116F10)
  ctx.r3.u64 = AddressOf(Element<ItemRecord>(Tables()->items, ctx.r4.u32));
}

// DataTables::Entry56: r3 the table (0 to 7), r4 the entry.
extern "C" REX_FUNC(sub_821172F8) {
  SBX_DATA_TABLES_FALLBACK(sub_821172F8)
  const uint32_t table = *Element<u32_be>(AddressOf(Tables()->tables_56), ctx.r3.u32);
  ctx.r3.u64 = AddressOf(Element<DataEntry56>(table, ctx.r4.u32));
}

// DataTables::Find20: r3 the key at +8, r4 the key at +0; the first such
// entry's index in asset 95's table, or -1.
extern "C" REX_FUNC(sub_82117318) {
  SBX_DATA_TABLES_FALLBACK(sub_82117318)
  const int32_t key_8 = ctx.r3.s32;
  const int32_t key_0 = ctx.r4.s32;
  int64_t found = -1;
  const uint32_t table = Tables()->table_20;
  for (int32_t i = 0;; i++) {
    const DataEntry20* entry = Element<DataEntry20>(table, uint32_t(i));
    if (entry->key_0 == -1) {
      break;
    }
    if (entry->key_8 == key_8 && entry->key_0 == key_0) {
      found = i;
      break;
    }
  }
  ctx.r3.s64 = found;
}

// DataTables::Entry20 and Entry40: r3 the entry.
extern "C" REX_FUNC(sub_82117378) {
  SBX_DATA_TABLES_FALLBACK(sub_82117378)
  ctx.r3.u64 = AddressOf(Element<DataEntry20>(Tables()->table_20, ctx.r3.u32));
}

extern "C" REX_FUNC(sub_82117390) {
  SBX_DATA_TABLES_FALLBACK(sub_82117390)
  ctx.r3.u64 = AddressOf(Element<DataEntry40>(Tables()->table_40, ctx.r3.u32));
}
