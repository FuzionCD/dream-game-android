#pragma once

#include <cstdint>

// AUTO-GENERATED from the Dream binary's master snag table at DAT_10007b348.
// 119 entries x 0x40 bytes each. layout matches the binary 1:1: 4 sprite ints +
// 1 tier int + 3 stat-multiplier floats + 4 (8-byte) string pointers = 0x40 bytes.
//
// regenerate with the snags.h extraction script in the project notes.

// SnagKind: name -> kind value mapping for every named snag in the bestiary.
// kind 0 ('None') is a sentinel placeholder; the binary's table[0] is all zeros.
// the 118 named entries (kinds 0x01..0x76) match the 117 sprites on the snag
// spritesheet plus the Doppelganger (0x19) which renders using the player's
// character-token sprite instead of a snag-sheet sprite.
enum class SnagKind : uint32_t {
    None = 0,
    Snag = 0x01,
    Hound = 0x02,
    Bite = 0x03,
    ChokingGrasp = 0x04,
    Mania = 0x05,
    Obsession = 0x06,
    Judgement = 0x07,
    Regret = 0x08,
    TunnelVision = 0x09,
    DejaVu = 0x0a,
    Claustrophobia = 0x0b,
    GapingMaw = 0x0c,
    Nostalgia = 0x0d,
    Hubris = 0x0e,
    Reluctance = 0x0f,
    Procrastination = 0x10,
    Doubt = 0x11,
    ShredOfDoubt = 0x12,
    Passion = 0x13,
    Rage = 0x14,
    Darkness = 0x15,
    ColdLogic = 0x16,
    ImpendingDoom = 0x17,
    DarkOmen = 0x18,
    Doppelganger = 0x19,
    Stranger = 0x1a,
    Malaise = 0x1b,
    Panic = 0x1c,
    Emptiness = 0x1d,
    EerieSilence = 0x1e,
    PiercingGaze = 0x1f,
    Stubbornness = 0x20,
    AlarmBells = 0x21,
    DistantEcho = 0x22,
    Vanity = 0x23,
    Indecision = 0x24,
    SuddenTension = 0x25,
    BrokenPromise = 0x26,
    Despair = 0x27,
    Fear = 0x28,
    Legion = 0x29,
    HauntingVisage = 0x2a,
    Cruelty = 0x2b,
    MentalBlock = 0x2c,
    Paranoia = 0x2d,
    Pride = 0x2e,
    FalseHope = 0x2f,
    CreepingDread = 0x30,
    Sloth = 0x31,
    Flashback = 0x32,
    Traitor = 0x33,
    Greed = 0x34,
    Ennui = 0x35,
    RepressedMemory = 0x36,
    Pity = 0x37,
    Whimsy = 0x38,
    Change = 0x39,
    BlindingLight = 0x3a,
    Power = 0x3b,
    Envy = 0x3c,
    Malice = 0x3d,
    Bluff = 0x3e,
    Terror = 0x3f,
    Ignorance = 0x40,
    SlowPoison = 0x41,
    Tradition = 0x42,
    Confusion = 0x43,
    Revenge = 0x44,
    Loneliness = 0x45,
    HeavyBurden = 0x46,
    Infestation = 0x47,
    Parasite = 0x48,
    Scapegoat = 0x49,
    Liar = 0x4a,
    Lie = 0x4b,
    Drama = 0x4c,
    Tragedy = 0x4d,
    Comedy = 0x4e,
    Apathy = 0x4f,
    Overconfidence = 0x50,
    Discord = 0x51,
    Grief = 0x52,
    Fatigue = 0x53,
    Frenzy = 0x54,
    DullPain = 0x55,
    SharpPain = 0x56,
    PainfulMemory = 0x57,
    Complacency = 0x58,
    Defiance = 0x59,
    Surprise = 0x5a,
    Incompetence = 0x5b,
    Masochism = 0x5c,
    HiddenAgenda = 0x5d,
    Suspicion = 0x5e,
    Sadism = 0x5f,
    Glory = 0x60,
    Distrust = 0x61,
    LosingStreak = 0x62,
    Myopia = 0x63,
    Neglect = 0x64,
    Outrage = 0x65,
    Stagnation = 0x66,
    MissedOpportunity = 0x67,
    Stress = 0x68,
    Zeal = 0x69,
    SharpShock = 0x6a,
    Trouble = 0x6b,
    Honesty = 0x6c,
    StalkingHorse = 0x6d,
    Vice = 0x6e,
    VeiledThreat = 0x6f,
    Attrition = 0x70,
    Bitterness = 0x71,
    Authority = 0x72,
    Mockery = 0x73,
    Guidance = 0x74,
    Tantrum = 0x75,
    SpiritDrain = 0x76,
};

// SnagInfo mirrors the binary's per-kind 0x40-byte struct layout. pointer width
// matches the binary's arm64 (8-byte ptrs); on 64-bit Android we get the same
// sizeof. not used as a packed memory layout (we never cast to/from binary
// bytes); it's a pure data record consumed by SnagContent and the snag draw paths.
struct SnagInfo {
    int         spriteU, spriteV;   // pixel coords on the snag spritesheet
    int         spriteW, spriteH;   // pixel dimensions
    int         tier;               // 0 = spawn-only, 1 = common, 2 = special, 3 = boss/elite
    float       atkMult;            // baseStat -> atk scale (only used when ctor
    float       defMult;            //   doesn't override stats; see special-case kinds below)
    float       hpMult;
    const char* name;
    const char* desc1;              // typically 'While Held:' / 'When Placed:' or generic
    const char* desc2;              // continuation OR a second behavior
    const char* desc3;              // 'When Defeated:' / 'When Drawn:' / etc; often empty
};

// the bestiary itself. constexpr-inline so the storage is in one place across
// all TUs that include this header.
inline constexpr SnagInfo SNAG_TABLE[119] = {
    /* 0x00
     */ {    0,    0,    0,    0, 0, 1.000f, 1.000f, 1.000f,
        ""                        , "",
        ""                        , "" },
    /* 0x01 Snag  // stats RNG-rolled in 4 distribution patterns instead of using the table multipliers
     */ {  350,    0,   72,   90, 1, 1.000f, 1.000f, 1.000f,
        "Snag"                    , "Attacks each turn until defeated.",
        "No special behaviour."   , "" },
    /* 0x02 Hound
     */ {    0,    0,   90,   91, 2, 2.200f, 1.000f, 1.500f,
        "Hound"                   , "When Placed: Moves to the start",
        "of the trail."           , "" },
    /* 0x03 Bite
     */ {  264,    0,   85,   90, 2, 2.000f, 1.500f, 1.200f,
        "Bite"                    , "While Held: Discard a random",
        "non-snag tile each turn.", "When Placed: Discard all non-snag tiles." },
    /* 0x04 Choking Grasp
     */ {   91,    0,   86,   91, 2, 1.500f, 2.000f, 1.300f,
        "Choking Grasp"           , "While Held: Freezes a random tile each turn.",
        "Frozen tiles can't be placed or discarded.", "" },
    /* 0x05 Mania
     */ {  513,    0,   87,   94, 2, 1.300f, 1.800f, 2.000f,
        "Mania"                   , "While Held: Swaps your {A} and {D} each turn.",
        "While Placed: Swaps its own {A} and {D} each turn.", "" },
    /* 0x06 Obsession  // writes 0x64 / 2 to the consumedFlag region
     */ {  178,    0,   85,   90, 0, 0.600f, 0.800f, 0.400f,
        "Obsession"               , "While Held: Can't discard snags.",
        "When Defeated: %d%% chance to be", "drawn again after %d turns." },
    /* 0x07 Judgement
     */ {  601,    0,   90,   92, 2, 0.400f, 2.000f, 1.200f,
        "Judgement"               , "While Placed: Gains 1 {A} per exit",
        "on its current tile each turn.", "Doesn't lose {A} when attacking." },
    /* 0x08 Regret
     */ {   86,  185,   92,   92, 2, 1.000f, 1.500f, 2.000f,
        "Regret"                  , "When Placed: Its {A} is set to the",
        "current trail length."   , "" },
    /* 0x09 Tunnel Vision
     */ {    0,  185,   85,   92, 2, 1.800f, 1.800f, 1.200f,
        "Tunnel Vision"           , "While Held: Can only place tiles with",
        "2 exits, or this tile."  , "" },
    /* 0x0a Deja Vu
     */ {  261,  184,   79,   95, 2, 1.700f, 2.000f, 1.300f,
        "Deja Vu"                 , "When Placed: Comes back to held tiles",
        "after attacking, unless defeated.", "" },
    /* 0x0b Claustrophobia
     */ {  692,    0,   91,   92, 2, 1.200f, 1.600f, 1.300f,
        "Claustrophobia"          , "While Placed: Doubles its {A} and {D} each",
        "turn you stand on a tile with 2 exits.", "" },
    /* 0x0c Gaping Maw
     */ {  793,   90,   79,   91, 2, 1.800f, 1.400f, 1.100f,
        "Gaping Maw"              , "While Held: Consumes the contents",
        "of the nearest held tile each turn.", "" },
    /* 0x0d Nostalgia
     */ {  432,  187,   84,   92, 2, 1.800f, 0.300f, 0.800f,
        "Nostalgia"               , "When Placed: Resets your {A} to zero.",
        ""                        , "" },
    /* 0x0e Hubris
     */ {  341,  184,   90,   86, 2, 0.700f, 2.400f, 1.600f,
        "Hubris"                  , "When Placed: Resets your {D} to zero.",
        ""                        , "" },
    /* 0x0f Reluctance  // writes 2 to consumedFlag
     */ {  695,  185,   89,   94, 2, 1.500f, 1.800f, 1.100f,
        "Reluctance"              , "While Placed: Chases only every 2nd turn.",
        ""                        , "" },
    /* 0x10 Procrastination
     */ {  179,  184,   81,   93, 2, 1.500f, 1.600f, 1.000f,
        "Procrastination"         , "Adds 2 {H} to all snags each turn.",
        ""                        , "" },
    /* 0x11 Doubt  // writes 0 to consumedFlag (creates Shred of Doubt while placed)
     */ {    0,  278,   86,   91, 2, 1.400f, 1.500f, 1.000f,
        "Doubt"                   , "While Held: Draw Shred of Doubt every 2nd turn.",
        "While Placed: Creates a Shred of Doubt on a", "random tile every 2nd turn." },
    /* 0x12 Shred of Doubt
     */ {  867,  185,   75,   76, 0, 0.300f, 0.300f, 0.200f,
        "Shred of Doubt"          , "Doesn't lose {A} or {D} when attacking.",
        "On attack, reduces your {A} and {D} by its", "{A} and {D} values, respectively." },
    /* 0x13 Passion
     */ {   87,  278,   95,   90, 2, 1.000f, 2.000f, 1.600f,
        "Passion"                 , "Gains 2 {A} each turn.",
        ""                        , "" },
    /* 0x14 Rage
     */ {  437,   95,   85,   91, 2, 0.700f, 3.000f, 1.400f,
        "Rage"                    , "While Held: Converts its {D} to {A}",
        "at 3 points per turn."   , "" },
    /* 0x15 Darkness
     */ {  785,  185,   81,   92, 2, 1.800f, 1.800f, 1.500f,
        "Darkness"                , "The effects of newly drawn tiles are hidden",
        "for 1 turn or until they're placed.", "" },
    /* 0x16 Cold Logic
     */ {  615,   93,   87,   91, 2, 1.500f, 2.400f, 1.600f,
        "Cold Logic"              , "While Held: Can't place {C} tiles.",
        ""                        , "" },
    /* 0x17 Impending Doom
     */ {  877,    0,   91,   91, 2, 1.800f, 0.500f, 1.200f,
        "Impending Doom"          , "When Placed: Nemesis advances to this tile.",
        ""                        , "" },
    /* 0x18 Dark Omen
     */ {  601,  187,   93,   90, 2, 1.000f, 3.000f, 1.600f,
        "Dark Omen"               , "While Placed: Nemesis advances 1 tile per turn.",
        ""                        , "" },
    /* 0x19 Doppelganger  // copies player's atk / def from PlayerSystem; calls FUN_100056478 to set portrait UV (uses the player's character token sprite, not a snag-sheet UV)
     */ {    0,    0,    0,    0, 2, 1.000f, 1.000f, 1.800f,
        "Doppelganger"            , "Matches your {A} and {D} each turn.",
        ""                        , "" },
    /* 0x1a Stranger
     */ {  523,   95,   91,   91, 1, 1.300f, 1.000f, 0.600f,
        "Stranger"                , "When Placed: Draw another special snag.",
        ""                        , "" },
    /* 0x1b Malaise
     */ {  271,  280,   84,   93, 2, 1.500f, 2.500f, 1.200f,
        "Malaise"                 , "When Defeated: Comes back with",
        "its {H} equal to its {D}", "" },
    /* 0x1c Panic
     */ {   90,   92,   90,   87, 2, 1.700f, 1.800f, 1.300f,
        "Panic"                   , "While Held: Adds 1 {A} to all snags each turn.",
        ""                        , "" },
    /* 0x1d Emptiness
     */ {  353,   92,   83,   91, 2, 1.000f, 3.000f, 1.800f,
        "Emptiness"               , "When Defeated: Discard all held tiles.",
        ""                        , "" },
    /* 0x1e Eerie Silence
     */ {  423,    0,   87,   91, 2, 1.800f, 2.200f, 1.600f,
        "Eerie Silence"           , "While Held: Events don't gain charges.",
        ""                        , "" },
    /* 0x1f Piercing Gaze
     */ {  873,   92,   92,   92, 2, 1.000f, 1.600f, 1.800f,
        "Piercing Gaze"           , "Ignores your {D} when attacking.",
        ""                        , "" },
    /* 0x20 Stubbornness
     */ {  356,  280,   87,   91, 2, 1.700f, 0.500f, 2.500f,
        "Stubbornness"            , "After each attack, its {D} is set",
        "to the {H} it lost in that attack.", "Can only be defeated if its {H} is 1." },
    /* 0x21 Alarm Bells
     */ {  607,  278,   83,   91, 2, 1.200f, 2.000f, 1.500f,
        "Alarm Bells"             , "While Held: Gains 1 {A} per turn.",
        "When Placed: Draw Distant Echo.", "" },
    /* 0x22 Distant Echo
     */ {  943,  185,   74,   80, 1, 1.400f, 0.400f, 1.100f,
        "Distant Echo"            , "While Held: Can't discard snags.",
        "When Defeated: 50% chance to", "draw another Distant Echo." },
    /* 0x23 Vanity
     */ {   89,  369,   93,   80, 2, 1.500f, 3.000f, 0.500f,
        "Vanity"                  , "Can only be placed while holding",
        "at least 3 {H} tiles."   , "" },
    /* 0x24 Indecision  // calls FUN_100013870 on the parent tile (push decoration) when constructed
     */ {  348,  374,   91,   93, 2, 1.800f, 1.200f, 2.000f,
        "Indecision"              , "While Held: Freezes a random snag each turn.",
        "Frozen tiles can't be placed or discarded.", "" },
    /* 0x25 Sudden Tension
     */ {  784,    0,   92,   89, 2, 1.400f, 3.000f, 1.600f,
        "Sudden Tension"          , "You cannot activate events.",
        ""                        , "" },
    /* 0x26 Broken Promise
     */ {    0,  551,   82,   93, 2, 1.800f, 1.700f, 1.100f,
        "Broken Promise"          , "When Placed: Convert all held tiles to blanks.",
        ""                        , "" },
    /* 0x27 Despair  // writes 1 to consumedFlag
     */ {  266,   91,   86,   92, 2, 2.000f, 1.400f, 0.800f,
        "Despair"                 , "While Held: Draw 1 blank tile per turn.",
        ""                        , "" },
    /* 0x28 Fear
     */ {  267,  374,   80,   91, 2, 1.800f, 1.800f, 1.000f,
        "Fear"                    , "Creates a threat token on a random exit",
        "each turn. Draw a snag when a tile is", "placed on the threat token." },
    /* 0x29 Legion
     */ {    0,   92,   89,   92, 2, 1.600f, 2.500f, 1.800f,
        "Legion"                  , "While Held: Can't discard snags.",
        "Can only be placed while not holding other snags.", "" },
    /* 0x2a Haunting Visage
     */ {  691,  280,   89,   92, 2, 1.600f, 1.600f, 1.600f,
        "Haunting Visage"         , "Freezes the middle held tile each turn.",
        "Frozen tiles can't be placed or discarded.", "" },
    /* 0x2b Cruelty
     */ {  183,  280,   87,   93, 2, 1.700f, 0.800f, 1.500f,
        "Cruelty"                 , "While Held: Adds a 3 damage trap to a random",
        "held tile each turn. Placing a trapped tile reduces", "your {H} by the trap's damage value." },
    /* 0x2c Mental Block
     */ {  524,  373,   80,   91, 2, 1.200f, 3.000f, 1.700f,
        "Mental Block"            , "When Drawn: Also draw a Block tile.",
        ""                        , "" },
    /* 0x2d Paranoia
     */ {  703,   93,   89,   91, 2, 1.700f, 2.500f, 1.200f,
        "Paranoia"                , "While Held: Converts a random held",
        "tile to Caution each turn.", "" },
    /* 0x2e Pride
     */ {  440,  373,   83,   91, 2, 1.500f, 1.500f, 1.000f,
        "Pride"                   , "Gains half of the {A} / {D} / {H} value",
        "from placed {A} / {D} / {H} tiles.", "" },
    /* 0x2f False Hope
     */ {  517,  187,   83,   92, 2, 0.600f, 2.000f, 1.200f,
        "False Hope"              , "When Placed: You gain {D} equal to",
        "your {H}, then your {H} is set to 1.", "" },
    /* 0x30 Creeping Dread
     */ {  181,   91,   84,   92, 2, 1.200f, 2.000f, 1.500f,
        "Creeping Dread"          , "While Held: Discard the tile to the right of",
        "this each turn. Gains 5 {A} each turn if it's", "the rightmost held tile." },
    /* 0x31 Sloth
     */ {    0,  370,   84,   92, 2, 1.500f, 0.700f, 2.300f,
        "Sloth"                   , "While Held: Can't rotate placed tiles.",
        ""                        , "" },
    /* 0x32 Flashback
     */ {  364,  468,   92,   91, 2, 1.800f, 2.300f, 0.900f,
        "Flashback"               , "Moves to the start of the",
        "trail after each attack.", "" },
    /* 0x33 Traitor
     */ {  183,  374,   83,   91, 2, 1.800f, 1.600f, 1.000f,
        "Traitor"                 , "While Held: Adds a 2 damage trap to the left and",
        "right held tile each turn. Placing a trapped tile", "reduces your {H} by the trap's damage value." },
    /* 0x34 Greed
     */ {  524,  280,   82,   92, 2, 1.300f, 3.000f, 1.000f,
        "Greed"                   , "While Held: Can't discard tiles.",
        ""                        , "" },
    /* 0x35 Ennui
     */ {  781,  280,   78,   91, 2, 1.600f, 1.800f, 1.500f,
        "Ennui"                   , "While Held: Convert a random held tile",
        "to a blank each turn."   , "Gains 1 {H} per held blank tile each turn." },
    /* 0x36 Repressed Memory
     */ {  275,  468,   88,   81, 2, 1.200f, 2.000f, 1.600f,
        "Repressed Memory"        , "When Placed: Draw 2 Block tiles.",
        ""                        , "" },
    /* 0x37 Pity
     */ {  457,  465,   87,   91, 2, 1.000f, 3.000f, 1.400f,
        "Pity"                    , "While Held: Adds a 2 damage trap to itself",
        "each turn. Placing a trapped tile reduces", "your {H} by the trap's damage value." },
    /* 0x38 Whimsy
     */ {  726,  468,   84,   90, 2, 1.600f, 1.000f, 1.800f,
        "Whimsy"                  , "Turns held tiles upside down",
        "every other turn."       , "" },
    /* 0x39 Change
     */ {  255,  558,   77,   93, 2, 1.100f, 2.000f, 1.700f,
        "Change"                  , "While Held: Held tiles are upside down.",
        ""                        , "" },
    /* 0x3a Blinding Light
     */ {  860,  278,   75,   91, 2, 1.900f, 1.700f, 1.300f,
        "Blinding Light"          , "While Held: Draw blanks instead of {H} tiles.",
        ""                        , "" },
    /* 0x3b Power
     */ {  811,  468,   94,   94, 3, 2.500f, 3.000f, 2.000f,
        "Power"                   , "Is unusually strong.",
        ""                        , "" },
    /* 0x3c Envy
     */ {  787,  372,   76,   95, 2, 1.200f, 1.400f, 1.000f,
        "Envy"                    , "While Held: Gains 1 {A} {H} {D} per tile you draw.",
        ""                        , "" },
    /* 0x3d Malice
     */ {  936,  266,   81,   92, 2, 1.500f, 0.600f, 1.800f,
        "Malice"                  , "When Placed: Reduce your {H} by your {A}",
        ""                        , "" },
    /* 0x3e Bluff
     */ {  697,  373,   89,   93, 2, 5.000f, 2.200f, 1.000f,
        "Bluff"                   , "While Held: Loses half of its {A}",
        "each time another snag is placed.", "" },
    /* 0x3f Terror
     */ {  942,  359,   77,   91, 3, 1.100f, 0.500f, 1.600f,
        "Terror"                  , "When Placed: Set your {H} to 1,",
        "Nemesis advances to current tile.", "" },
    /* 0x40 Ignorance
     */ {   98,  450,   82,   90, 2, 1.500f, 2.000f, 1.800f,
        "Ignorance"               , "Draw only tiles with 2 exits.",
        ""                        , "" },
    /* 0x41 Slow Poison
     */ {  181,  466,   93,   91, 2, 1.300f, 1.700f, 1.400f,
        "Slow Poison"             , "While Held: Reduces your {D} by the number",
        "of held snags each turn. If your {D} is 0,", "halves your {H} each turn." },
    /* 0x42 Tradition
     */ {   83,  551,   79,   92, 2, 1.800f, 1.800f, 1.800f,
        "Tradition"               , "While Held: Freezes the left tile each turn.",
        "While Placed: Freezes the right tile each turn.", "Frozen tiles can't be placed or discarded." },
    /* 0x43 Confusion
     */ {  545,  465,   86,   91, 2, 1.700f, 2.000f, 1.400f,
        "Confusion"               , "While Held: Placed tile rotates continuously.",
        ""                        , "" },
    /* 0x44 Revenge
     */ {  906,  463,   83,   94, 2, 1.800f, 0.200f, 2.000f,
        "Revenge"                 , "When Defeated: Adds a trap to each held tile",
        "equal to Revenge's {H} before the final attack.", "Placing a trapped tile reduces your {H} by its value." },
    /* 0x45 Loneliness
     */ {    0,  462,   97,   88, 2, 1.200f, 3.200f, 1.600f,
        "Loneliness"              , "While Held: Draw blanks instead of normal snags,",
        "Nemesis gains 1 XP per blank you hold per turn.", "" },
    /* 0x46 Heavy Burden
     */ {  864,  370,   77,   92, 2, 1.400f, 2.600f, 1.200f,
        "Heavy Burden"            , "When Placed: Draw a value 9 Dead Weight.",
        ""                        , "" },
    /* 0x47 Infestation  // clamps def to (snags / 35) clamped [5..15]
     */ {  425,  560,   77,   90, 2, 1.500f, 0.000f, 1.600f,
        "Infestation"             , "Can only be placed if its {D} is 0.",
        "Draw Parasites instead of {D} tiles.", "While Held: Loses 1 {D} per tile you discard." },
    /* 0x48 Parasite
     */ {  503,  557,   88,   85, 0, 0.400f, 0.300f, 0.200f,
        "Parasite"                , "While Held: Draw Parasites instead of {D} tiles.",
        "Doesn't reduce your {A} on attack.", "" },
    /* 0x49 Scapegoat
     */ {  333,  560,   91,   92, 2, 0.300f, 1.700f, 0.400f,
        "Scapegoat"               , "Can only be placed while holding 5 snags.",
        "Can be discarded, which levels up Nemesis.", "" },
    /* 0x4a Liar
     */ {  163,  558,   91,   90, 1, 1.300f, 1.500f, 1.100f,
        "Liar"                    , "When Defeated: Draw 1 - 3 Lies.",
        ""                        , "" },
    /* 0x4b Lie
     */ {  592,  561,   82,   90, 1, 1.300f, 0.800f, 0.500f,
        "Lie"                     , "While Held: Can't see its stats.",
        ""                        , "" },
    /* 0x4c Drama
     */ {  839,  563,   98,   92, 1, 1.600f, 1.600f, 0.600f,
        "Drama"                   , "When Defeated:",
        "Draw Tragedy with {A} {H} {D} equal to Drama's {A}", "Draw Comedy with {A} {H} {D} equal to Drama's {D}" },
    /* 0x4d Tragedy
     */ {  675,  561,   82,   92, 1, 1.000f, 1.000f, 1.000f,
        "Tragedy"                 , "While Held: Adds 1 {A} to",
        "all held snags each turn.", "" },
    /* 0x4e Comedy
     */ {  758,  563,   80,   91, 1, 1.000f, 1.000f, 1.000f,
        "Comedy"                  , "While Held: Adds 1 {D} to",
        "all held snags each turn.", "" },
    /* 0x4f Apathy
     */ {    0,  742,   89,   93, 2, 1.700f, 1.700f, 1.200f,
        "Apathy"                  , "When Placed: Lose half of your {A} {H} {D}",
        ""                        , "" },
    /* 0x50 Overconfidence
     */ {  270,  836,   80,   94, 2, 1.900f, 0.800f, 1.700f,
        "Overconfidence"          , "While Held: Gains {D} equal to",
        "your current {X} each turn.", "" },
    /* 0x51 Discord
     */ {  865,  656,   76,   94, 2, 1.200f, 2.300f, 1.300f,
        "Discord"                 , "When Placed: Set your {C} to 0.",
        ""                        , "" },
    /* 0x52 Grief  // forces def >= 3
     */ {  697,  655,   81,   93, 2, 1.800f, 0.800f, 1.700f,
        "Grief"                   , "Can only be placed if its {D}",
        "is higher than your {A}" , "" },
    /* 0x53 Fatigue
     */ {  604,  654,   92,   90, 2, 1.400f, 1.800f, 1.500f,
        "Fatigue"                 , "When Defeated: Set your {A} & {D} to 0.",
        ""                        , "" },
    /* 0x54 Frenzy
     */ {  428,  651,   86,   93, 2, 1.600f, 0.500f, 1.800f,
        "Frenzy"                  , "While Placed: Draw only {A} tiles,",
        "can't discard tiles."    , "" },
    /* 0x55 Dull Pain
     */ {  444,  280,   79,   92, 2, 1.800f, 2.500f, 1.100f,
        "Dull Pain"               , "While Held: Convert a random held tile",
        "to a value 2 Pain each turn.", "" },
    /* 0x56 Sharp Pain
     */ {  632,  467,   93,   93, 2, 2.000f, 1.500f, 0.800f,
        "Sharp Pain"              , "When Placed: Draw a Pain tile of",
        "value equal to your max {H}", "" },
    /* 0x57 Painful Memory
     */ {    0,  836,   84,   91, 2, 1.400f, 2.600f, 1.200f,
        "Painful Memory"          , "When Defeated: Convert all held tiles to Pain.",
        ""                        , "" },
    /* 0x58 Complacency
     */ {  180,  931,   91,   89, 2, 1.500f, 2.000f, 1.400f,
        "Complacency"             , "While Placed: Attacks only if its {H}",
        "is higher than yours."   , "" },
    /* 0x59 Defiance
     */ {  515,  652,   88,   93, 2, 2.400f, 1.600f, 1.000f,
        "Defiance"                , "While Held: Loses 1 {A} per tile you discard.",
        ""                        , "" },
    /* 0x5a Surprise
     */ {  605,  373,   91,   91, 0, 0.500f, 0.500f, 0.300f,
        "Surprise"                , "When Defeated: Draw a special snag.",
        ""                        , "" },
    /* 0x5b Incompetence
     */ {  257,  653,   92,   92, 2, 1.800f, 2.000f, 1.100f,
        "Incompetence"            , "Draw a blank each time you discard.",
        ""                        , "" },
    /* 0x5c Masochism
     */ {  272,  931,   76,   91, 2, 0.600f, 0.000f, 2.200f,
        "Masochism"               , "While Placed: Gains {A} equal to your {A} each turn.",
        "When attacked, loses {H} equal to its {A}, instead", "of taking damage from your attack." },
    /* 0x5d Hidden Agenda
     */ {   83,  928,   96,   96, 2, 1.600f, 2.200f, 1.300f,
        "Hidden Agenda"           , "When Defeated: Draw 2 Secrets of values",
        "equal to Hidden Agenda's {A} and {D}", "" },
    /* 0x5e Suspicion
     */ {   90,  649,   91,   99, 2, 1.400f, 1.800f, 1.600f,
        "Suspicion"               , "When Placed: Convert held {A} {H} {D} {C} tiles to",
        "Secrets of value equal to double their value.", "" },
    /* 0x5f Sadism
     */ {  182,  652,   74,   90, 2, 1.800f, 0.800f, 2.000f,
        "Sadism"                  , "When Attacked: Draw a Pain tile of value",
        "equal to damage done to Sadism.", "" },
    /* 0x60 Glory
     */ {  942,  653,   76,   92, 2, 1.600f, 0.600f, 2.500f,
        "Glory"                   , "When Defeated: Draw a Dead Weight of value",
        "equal to Glory's {H} before the final attack.", "" },
    /* 0x61 Distrust
     */ {  351,  746,   88,   92, 2, 1.400f, 2.000f, 1.200f,
        "Distrust"                , "When Defeated: Draw 2 Dead Weights",
        "of random values between 1 and 7.", "" },
    /* 0x62 Losing Streak
     */ {  350,  653,   77,   92, 2, 1.500f, 1.800f, 1.200f,
        "Losing Streak"           , "When Placed: Draw 1-6 Bad Luck tiles.",
        ""                        , "" },
    /* 0x63 Myopia
     */ {   90,  750,   90,   89, 2, 1.900f, 1.800f, 1.400f,
        "Myopia"                  , "While Held: Can't see other held snags' stats.",
        ""                        , "" },
    /* 0x64 Neglect
     */ {   85,  840,   94,   86, 2, 2.000f, 1.300f, 1.700f,
        "Neglect"                 , "While Held: Draw a blank when",
        "you place a {A} or {D} tile.", "" },
    /* 0x65 Outrage
     */ {  779,  656,   85,   92, 2, 1.200f, 2.700f, 1.400f,
        "Outrage"                 , "While Held: Can't place {D} tiles.",
        ""                        , "" },
    /* 0x66 Stagnation
     */ {  182,  746,   81,   93, 2, 1.600f, 2.000f, 1.000f,
        "Stagnation"              , "While Held: Gains 1 {H} per",
        "event charge each turn." , "" },
    /* 0x67 Missed Opportunity
     */ {  180,  840,   89,   90, 2, 1.400f, 1.800f, 1.300f,
        "Missed Opportunity"      , "When Placed: Discard a random event.",
        ""                        , "" },
    /* 0x68 Stress
     */ {    0,  928,   82,   91, 2, 1.200f, 1.200f, 2.000f,
        "Stress"                  , "While Held: Gains 1 {A} {D} per",
        "held snag each turn."    , "" },
    /* 0x69 Zeal
     */ {    0,  645,   89,   96, 2, 2.000f, 1.600f, 1.300f,
        "Zeal"                    , "Can't discard blank tiles.",
        "While Held: Draw blanks instead of {C} tiles.", "" },
    /* 0x6a Sharp Shock
     */ {  264,  746,   86,   89, 2, 1.800f, 2.000f, 1.400f,
        "Sharp Shock"             , "When Placed: Draw a Block tile.",
        "While Placed: Freezes the right tile each turn.", "Frozen tiles can't be placed or discarded." },
    /* 0x6b Trouble
     */ {  938,  558,   84,   94, 2, 1.500f, 2.300f, 1.300f,
        "Trouble"                 , "When Placed: Creates threat tokens around the",
        "current tile. Draw a snag when a tile is placed", "on a threat token." },
    /* 0x6c Honesty  // forces atk = 0, def = 0, hp = 1 (the passive 'Honesty' snag, gains 1 HP per turn while placed)
     */ {  351,  839,   82,   93, 0, 1.000f, 1.000f, 1.000f,
        "Honesty"                 , "When Placed: Moves to the start of the trail.",
        "While Placed: Gains 1 {H} per turn.", "You gain Honesty's {H} in {A} when it reaches you." },
    /* 0x6d Stalking Horse
     */ {  521,  838,   70,   91, 1, 0.700f, 1.000f, 0.500f,
        "Stalking Horse"          , "Can only be placed while holding",
        "at least 2 special snags.", "" },
    /* 0x6e Vice
     */ {  613,  745,   80,   91, 2, 1.400f, 2.000f, 1.200f,
        "Vice"                    , "While Held: Convert all held",
        "{D} tiles to blanks each turn.", "" },
    /* 0x6f Veiled Threat
     */ {  349,  933,   92,   91, 2, 1.300f, 1.800f, 1.400f,
        "Veiled Threat"           , "When Placed: Creates threat tokens",
        "around the exit. Draw a snag when", "a tile is placed on a threat token." },
    /* 0x70 Attrition  // hp clamped to [5..10] and atk forced >= 2
     */ {  592,  838,   89,   92, 3, 0.600f, 0.800f, 1.200f,
        "Attrition"               , "While Placed: Takes at most 1 damage",
        "from each of your attacks.", "" },
    /* 0x71 Bitterness
     */ {  530,  746,   82,   91, 2, 1.500f, 1.800f, 1.100f,
        "Bitterness"              , "While Held: You lose 1 {H} per held special",
        "snag per turn. This can't reduce your {H} to 0.", "" },
    /* 0x72 Authority
     */ {  611,  933,   90,   91, 2, 1.000f, 2.300f, 1.300f,
        "Authority"               , "While Held: Resets your {A} to be",
        "no higher than your {D} each turn.", "" },
    /* 0x73 Mockery
     */ {  442,  931,   82,   91, 2, 1.400f, 0.800f, 2.300f,
        "Mockery"                 , "While Held: Drains 1 {X} from you per turn.",
        "Loses half its {H} per {X} drained.", "" },
    /* 0x74 Guidance
     */ {  434,  839,   86,   91, 2, 1.700f, 1.900f, 1.000f,
        "Guidance"                , "While Held: Placed tile may rotate randomly.",
        ""                        , "" },
    /* 0x75 Tantrum
     */ {  440,  746,   89,   90, 1, 1.500f, 1.300f, 1.000f,
        "Tantrum"                 , "While Held: Can only discard the rightmost",
        "tile, even if it's not normally discardable.", "" },
    /* 0x76 Spirit Drain
     */ {  525,  934,   85,   90, 2, 1.200f, 2.000f, 1.500f,
        "Spirit Drain"            , "When Placed: Replace your left",
        "event with Exhaustion."  , "" },
};

// helpers
inline const SnagInfo& snagInfo(SnagKind k)   { return SNAG_TABLE[static_cast<uint32_t>(k)]; }
inline const SnagInfo& snagInfo(uint32_t k)   { return SNAG_TABLE[k]; }

// special-case kinds whose stats / behavior are not a pure table lookup. these
// are handled by SnagContent::init's per-kind switch (FUN_10003ccc8 in the binary).
// listed here for searchability:
//   0x01  Snag            - stats RNG-rolled in 4 distribution patterns instead of using the table multipliers
//   0x06  Obsession       - writes 0x64 / 2 to the consumedFlag region (+0x490)
//   0x0f  Reluctance      - writes 2 to consumedFlag
//   0x11  Doubt           - writes 0 to consumedFlag (creates Shred of Doubt while placed)
//   0x19  Doppelganger    - copies player's atk / def from PlayerSystem; calls FUN_100056478 to set portrait UV (uses the player's character token sprite, not a snag-sheet UV)
//   0x24  Indecision      - calls FUN_100013870 on the parent tile (push decoration) when constructed
//   0x27  Despair         - writes 1 to consumedFlag
//   0x47  Infestation     - clamps def to (snags / 35) clamped [5..15]
//   0x52  Grief           - forces def >= 3
//   0x6c  Honesty         - forces atk = 0, def = 0, hp = 1 (the passive 'Honesty' snag, gains 1 HP per turn while placed)
//   0x70  Attrition       - hp clamped to [5..10] and atk forced >= 2
