#pragma once

// AUTO-EXTRACTED from the Dream binary's content text table at DAT_100078818.
// 26 entries x 4 string pointers each, embedded in the same per-content-type
// row as the UV / offset data exposed by tile_content.cpp::kContentTypeUVTable.
// strings live at row+0x30..+0x48; this header carries name + 3 description
// lines only. consumed by FUN_100014ce0 (name) and FUN_100014cfc (desc N),
// the DetailPanel content / generic-text populators.
//
// entries 0..1 are the sentinel rows the binary keeps for type 0 (no content)
// and type 1 (snag-tile marker). entries 2/3/5/6 are the four stat-pickup
// content types (ATK / DEF / CTRL / HP) whose names show only through the
// HUD's stat-burst path - the in-game DetailPanel never pops for them.
// the named, player-facing pickups are entries 4 + 7..25.

struct TileTextEntry {
    const char* name;
    const char* desc1;
    const char* desc2;
    const char* desc3;
};

inline constexpr TileTextEntry kTileTextTable[26] = {
    /*0x00*/ {"", "", "", ""}, // blank tile
    /*0x01*/ {"", "", "", ""}, // snag, special or regular
    /*0x02*/ {"", "", "", ""}, // DEF bonus tile
    /*0x03*/ {"", "", "", ""}, // ATK bonus tile
    /*0x04*/ {"Experience", "When this tile is consumed by Nemesis,",
              "you will gain {X} equal to this value",
              "and Nemesis will gain 1 experience"},
    /*0x05*/ {"", "", "", ""}, // CTRL tile
    /*0x06*/ {"", "", "", ""}, // HP tile
    /*0x07*/ {"Caution", "If you discard Caution, Nemesis gains",
              "experience equal to its value.", ""},
    /*0x08*/ {"Block", "Can't be placed or discarded.",
              "Discards automatically when",
              "this is the rightmost held tile."},
    /*0x09*/ {"Dead Weight", "Can't be placed or discarded.",
              "Turns into a blank after a number of turns.", ""},
    /*0x0a*/ {"Potential", "When Placed: Gain 1 {X} per",
              "adjacent placed tile, minus 1.", ""},
    /*0x0b*/ {"Discipline", "When Placed: Gain {D} equal to its value.",
              "Gains 1 point value per tile you discard.", ""},
    /*0x0c*/ {"Clarity", "When Placed: Gain {H} equal to its value.",
              "Gains 1 point value per held tile to the",
              "right of this tile per turn."},
    /*0x0d*/ {"Lure", "While Placed: Pulls snags towards it 2 tiles per",
              "turn, Nemesis advances 1 tile per turn.", ""},
    /*0x0e*/ {"Pause", "When Placed: Gain 1 {A} {D} per held Pause,",
              "placed snags don't move or attack this turn.", ""},
    /*0x0f*/ {"Barricade", "While Placed: Stops Nemesis advancing",
              "through this tile for 1 turn.", ""},
    /*0x10*/ {"Wealth", "When Placed: Gain 1 {C},",
              "lose 3 {A} for each held Wealth tile.", ""},
    /*0x11*/ {"Pain", "While Held: Gains 1 value each turn.",
              "When Placed: Lose {H} equal to Pain's value.",
              "When Discarded: Draw Pain of half this value."},
    /*0x12*/ {"Secret", "Can't be discarded.",
              "Loses 1 value per tile you discard.",
              "When Placed: Lose {A} {D} equal to Secret's value."},
    /*0x13*/ {"Bad Luck", "Can't be discarded.",
              "When Placed: Nemesis advances 1 tile per level.", ""},
    /*0x14*/ {"Faith", "When Placed: Gain 2 {A} per held Faith.", "", ""},
    /*0x15*/ {"Foresight", "When Placed: You don't lose {A} {H} {D}",
              "from snag attacks this turn.", ""},
    /*0x16*/ {"Memento", "When Placed: Gain {A} {H} {D}",
              "equal to Memento's value.", ""},
    /*0x17*/ {"Warmth", "While Placed: Normal & special snags placed",
              "adjacent to this tile lose half their {H}", ""},
    /*0x18*/ {"Effort", "When Placed: Gain {A} {D} equal to Effort's value.",
              "Gains 1 value each turn by draining 1 or 3 value",
              "from 2 or 1 adjacent {A} {H} {D} {C} tiles."},
    /*0x19*/ {"Milestone", "While Placed: Gain 1 {H} each turn.",
              "The next placed snag will move here",
              "when placed and destroy this Milestone."},
};
