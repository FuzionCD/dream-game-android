#pragma once

// DEPRECATED: TileReserveNode was a hand-rolled std::list<TileReserveEntry>
// node. it has been replaced by `std::list<TileReserveEntry>` (declared in
// game_board.h). this header is kept as a tombstone so any stale includes
// fail loudly via the static_assert below rather than silently using a
// wrong-shaped type. delete the file once no .cpp file includes it anymore.

static_assert(false,
              "tile_reserve_node.h is deprecated; use std::list<TileReserveEntry> "
              "from game_board.h instead. remove the #include and rerun the build.");
