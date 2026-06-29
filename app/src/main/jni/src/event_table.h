#pragma once

#include <cstdint>

// AUTO-GENERATED from the Dream binary's master Event tables:
//   DAT_100079058: 81 x 0x28 per-eventType registry
//   DAT_100079008:  5 x 0x10 per-charge-category icon UV table

// charge category: which gameplay action fills the slot's charge bar.
// shared across all Events of the same category (e.g. every key=2 Event
// charges +1 when you place an {A} tile). 5 categories total.
enum class EventKey : uint32_t {
    Experience = 0,
    Health = 1,
    Attack = 2,
    Defence = 3,
    Control = 4,
};

// EventKind: name -> unique-event-identity mapping for all 81 named Events.
// each Kind is a distinct Event with its own name, description, and effect
// (dispatched by FUN_100020f80's per-kind switch). multiple Kinds may share
// the same EventKey if they're charged by the same gameplay action.
enum class EventKind : uint32_t {
    ShatteredMemory = 0,     // key=0 (Experience)
    FlashOfInsight = 1,     // key=4 (Control)
    SkeletonKey = 2,     // key=2 (Attack)
    HardenedShell = 3,     // key=3 (Defence)
    PrimalRoar = 4,     // key=1 (Health)
    NexusOfPower = 5,     // key=4 (Control)
    SinisterCrowd = 6,     // key=3 (Defence)
    CircleOfProtection = 7,     // key=0 (Experience)
    BorrowedTime = 8,     // key=1 (Health)
    Bait = 9,     // key=2 (Attack)
    Maelstrom = 10,     // key=1 (Health)
    SuckerPunch = 11,     // key=2 (Attack)
    ShiftingSands = 12,     // key=4 (Control)
    Metamorphosis = 13,     // key=0 (Experience)
    SoothingMelody = 14,     // key=3 (Defence)
    AWarmSmile = 15,     // key=1 (Health)
    Falling = 16,     // key=4 (Control)
    Checkmate = 17,     // key=0 (Experience)
    AMomentOfRest = 18,     // key=4 (Control)
    EarlyWarning = 19,     // key=2 (Attack)
    UntappedPotential = 20,     // key=4 (Control)
    SuddenPhoneCall = 21,     // key=3 (Defence)
    Interception = 22,     // key=2 (Attack)
    SubtleFragrance = 23,     // key=1 (Health)
    Gallop = 24,     // key=0 (Experience)
    LatentDiscipline = 25,     // key=3 (Defence)
    DeepClarity = 26,     // key=4 (Control)
    Pacifism = 27,     // key=3 (Defence)
    Tranquility = 28,     // key=1 (Health)
    GrindingGears = 29,     // key=0 (Experience)
    AlluringSigil = 30,     // key=2 (Attack)
    TowerOfCards = 31,     // key=1 (Health)
    BruteForce = 32,     // key=2 (Attack)
    InverseRefrain = 33,     // key=0 (Experience)
    FamiliarFaces = 34,     // key=4 (Control)
    ABriefPause = 35,     // key=3 (Defence)
    BarricadedDoor = 36,     // key=1 (Health)
    SublimeRadiance = 37,     // key=2 (Attack)
    AirSupport = 38,     // key=4 (Control)
    ALongRoad = 39,     // key=1 (Health)
    HiddenWealth = 40,     // key=0 (Experience)
    HypnoticGaze = 41,     // key=3 (Defence)
    Concentration = 42,     // key=1 (Health)
    GlaringHeadlights = 43,     // key=2 (Attack)
    Flying = 44,     // key=0 (Experience)
    UnlockedDoor = 45,     // key=0 (Experience)
    ConsolidatePower = 46,     // key=2 (Attack)
    StrongMedicine = 47,     // key=3 (Defence)
    Premonition = 48,     // key=4 (Control)
    WantonDestruction = 49,     // key=3 (Defence)
    OldFaith = 50,     // key=2 (Attack)
    LongRangeScan = 51,     // key=1 (Health)
    Rewind = 52,     // key=4 (Control)
    Overcharge = 53,     // key=0 (Experience)
    WellOiledMachine = 54,     // key=1 (Health)
    SweetMemento = 55,     // key=0 (Experience)
    WarmGlow = 56,     // key=4 (Control)
    Sideswipe = 57,     // key=3 (Defence)
    StrongVitals = 58,     // key=3 (Defence)
    Stampede = 59,     // key=2 (Attack)
    LuckyDraw = 60,     // key=1 (Health)
    HardWork = 61,     // key=0 (Experience)
    FlameBreath = 62,     // key=4 (Control)
    Snapshot = 63,     // key=3 (Defence)
    AmmoDump = 64,     // key=2 (Attack)
    Backtrack = 65,     // key=1 (Health)
    Disarm = 66,     // key=2 (Attack)
    SafetyInNumbers = 67,     // key=4 (Control)
    Breakthrough = 68,     // key=0 (Experience)
    FearlessDefence = 69,     // key=3 (Defence)
    Clocktower = 70,     // key=0 (Experience)
    IdyllicLandscape = 71,     // key=4 (Control)
    InnerStrength = 72,     // key=1 (Health)
    PuppetMaster = 73,     // key=0 (Experience)
    EmergencyBroadcast = 74,     // key=2 (Attack)
    Countdown = 75,     // key=2 (Attack)
    CovertStrike = 76,     // key=3 (Defence)
    SapphireCity = 77,     // key=3 (Defence)
    CostlyGift = 78,     // key=1 (Health)
    PureEfficiency = 79,     // key=4 (Control)
    Exhaustion = 80,     // key=2 (Attack)
};

// per-charge-category icon UV (from DAT_100079008). indexed by EventKey.
// mainQuad atlas      = (mainAtlasX, mainAtlasY),       size 131x102.
// chargeMarker atlas  = (markerAtlasX, markerAtlasY),   size 12x12.
//                       this is the per-key colored fill sprite that
//                       appears inside a chargeSlot when that slot fills.
//                       the chargeSlot itself (the always-visible 20x20
//                       backdrop) uses a single universal sprite at
//                       atlas (0, 929), fixed across all event keys.
struct EventKeyIconUV {
    int mainAtlasX;
    int mainAtlasY;
    int markerAtlasX;
    int markerAtlasY;
};

inline constexpr EventKeyIconUV EVENT_KEY_ICON_UV[5] = {
    {     0,     0,    73,   937 },   // key=0 Experience
    {     0,   103,    60,   937 },   // key=1 Health
    {     0,   206,    47,   937 },   // key=2 Attack
    {     0,   309,    21,   937 },   // key=3 Defence
    {     0,   412,    34,   937 },   // key=4 Control
};

// EventInfo mirrors the binary's per-eventType 0x28-byte record:
//   uint  key                : charge category (-> EventKeyIconUV)
//   uint  chargesMax         : charge slots needed to fire the event (<=7)
//   int   eventFrameAtlasX   : eventFrame icon X on items1 atlas (per-Event)
//   int   eventFrameAtlasY   : eventFrame icon Y on items1 atlas
//   char* name               : display name
//   char* desc[0..1]         : effect description (line 2 optional)
struct EventInfo {
    EventKey    key;
    uint32_t    chargesMax;
    int         eventFrameAtlasX;
    int         eventFrameAtlasY;
    const char* name;
    const char* desc[2];
};

inline constexpr EventInfo EVENT_TABLE[81] = {
    /*   0 ShatteredMemory */
    { EventKey::Experience, 3,  818,    0,
      "Shattered Memory",
      { "Discard all discardable held tiles.", "" } },
    /*   1 FlashOfInsight */
    { EventKey::Control, 2,  132,   58,
      "Flash of Insight",
      { "Discard any held tile.", "Does not affect special snags." } },
    /*   2 SkeletonKey */
    { EventKey::Attack, 5,  426,    0,
      "Skeleton Key",
      { "Double all held {A} tile values.", "" } },
    /*   3 HardenedShell */
    { EventKey::Defence, 5,  916,    0,
      "Hardened Shell",
      { "Double all held {D} tile values.", "" } },
    /*   4 PrimalRoar */
    { EventKey::Health, 5,  426,   58,
      "Primal Roar",
      { "Gain 10 {A}, {D} and {H}", "" } },
    /*   5 NexusOfPower */
    { EventKey::Control, 7,  720,    0,
      "Nexus of Power",
      { "Gain {C} equal to the number of", "exits on the current tile, minus 2." } },
    /*   6 SinisterCrowd */
    { EventKey::Defence, 5,  230,  116,
      "Sinister Crowd",
      { "Gain {X} per held special snag.", "" } },
    /*   7 CircleOfProtection */
    { EventKey::Experience, 3,  132,  174,
      "Circle of Protection",
      { "Discard all held {H} tiles.", "Draw {D} tile per tile discarded." } },
    /*   8 BorrowedTime */
    { EventKey::Health, 4,  328,  116,
      "Borrowed Time",
      { "Advance Nemesis to current tile.", "Gain {H} per tile it moves over." } },
    /*   9 Bait */
    { EventKey::Attack, 6,  328,  174,
      "Bait",
      { "Advance Nemesis into the next {X}", "Gain double {X} from it." } },
    /*  10 Maelstrom */
    { EventKey::Health, 5,  230,  174,
      "Maelstrom",
      { "Discard all your events.", "Gain {X} per event discarded." } },
    /*  11 SuckerPunch */
    { EventKey::Attack, 6,  426,  116,
      "Sucker Punch",
      { "Double your current {A}", "Set your current {H} to 1." } },
    /*  12 ShiftingSands */
    { EventKey::Control, 4,  524,  116,
      "Shifting Sands",
      { "Push all placed snags back 5 tiles.", "" } },
    /*  13 Metamorphosis */
    { EventKey::Experience, 3,  230,   58,
      "Metamorphosis",
      { "Discard a held special snag.", "Draw another special snag." } },
    /*  14 SoothingMelody */
    { EventKey::Defence, 3,  720,  116,
      "Soothing Melody",
      { "Blank a held normal snag tile.", "" } },
    /*  15 AWarmSmile */
    { EventKey::Health, 3,  524,   58,
      "A Warm Smile",
      { "Set your {H} to half your max {H}", "" } },
    /*  16 Falling */
    { EventKey::Control, 5,  720,   58,
      "Falling",
      { "Blank a random held tile.", "Gain triple value if it's {A} {H} {D} {C}" } },
    /*  17 Checkmate */
    { EventKey::Experience, 5,  328,   58,
      "Checkmate",
      { "Defeat all placed normal snags.", "Gain 1 {C} per defeated snag." } },
    /*  18 AMomentOfRest */
    { EventKey::Control, 5,  132,    0,
      "A Moment of Rest",
      { "Halve your current {A}", "Double your current {D}" } },
    /*  19 EarlyWarning */
    { EventKey::Attack, 6,  132,  116,
      "Early Warning",
      { "Set a held normal snag's {H} to 1.", "" } },
    /*  20 UntappedPotential */
    { EventKey::Control, 7,  622,    0,
      "Untapped Potential",
      { "Convert a held tile into Potential.", "Potential gives {X} when placed." } },
    /*  21 SuddenPhoneCall */
    { EventKey::Defence, 5,  916,  232,
      "Sudden Phone Call",
      { "Convert a held special snag into a", "normal snag. Nemesis levels up." } },
    /*  22 Interception */
    { EventKey::Attack, 5,  916,   58,
      "Interception",
      { "Set your {A} equal to to your {D}", "" } },
    /*  23 SubtleFragrance */
    { EventKey::Health, 4,  132,  232,
      "Subtle Fragrance",
      { "Create 1 {X} at the start of the trail.", "" } },
    /*  24 Gallop */
    { EventKey::Experience, 3,  622,   58,
      "Gallop",
      { "Add 2 charges to all other events.", "" } },
    /*  25 LatentDiscipline */
    { EventKey::Defence, 6,  230,  290,
      "Latent Discipline",
      { "Draw a Discipline tile.", "Discipline grows its {D} value." } },
    /*  26 DeepClarity */
    { EventKey::Control, 5,  916,  116,
      "Deep Clarity",
      { "Draw a Clarity tile.", "Clarity grows its {H} value." } },
    /*  27 Pacifism */
    { EventKey::Defence, 6,  720,  232,
      "Pacifism",
      { "Refill your {H}, set your {A} to 0.", "" } },
    /*  28 Tranquility */
    { EventKey::Health, 5,  524,  174,
      "Tranquility",
      { "Discard all normal snags.", "" } },
    /*  29 GrindingGears */
    { EventKey::Experience, 5,  328,    0,
      "Grinding Gears",
      { "Gain 3 {C}", "Nemesis gains 5 experience." } },
    /*  30 AlluringSigil */
    { EventKey::Attack, 7,  818,  174,
      "Alluring Sigil",
      { "Draw a Lure tile. Lure pulls", "snags & Nemesis towards it." } },
    /*  31 TowerOfCards */
    { EventKey::Health, 5,  622,  116,
      "Tower of Cards",
      { "Create control spots around", "the current tile." } },
    /*  32 BruteForce */
    { EventKey::Attack, 4,  524,  232,
      "Brute Force",
      { "Gain 3 {A} per held snag.", "" } },
    /*  33 InverseRefrain */
    { EventKey::Experience, 3,  818,  116,
      "Inverse Refrain",
      { "Swap your {A} and {D}", "" } },
    /*  34 FamiliarFaces */
    { EventKey::Control, 4,  818,  232,
      "Familiar Faces",
      { "Multiply the value of held {D} tiles", "by the number of held snags." } },
    /*  35 ABriefPause */
    { EventKey::Defence, 5,  720,  174,
      "A Brief Pause",
      { "Convert selected tiles to Pauses.", "Pauses stop snags and give {A} {D}" } },
    /*  36 BarricadedDoor */
    { EventKey::Health, 3,  524,    0,
      "Barricaded Door",
      { "Draw a Barricade tile.", "Barricade stops Nemesis briefly." } },
    /*  37 SublimeRadiance */
    { EventKey::Attack, 4,  426,  174,
      "Sublime Radiance",
      { "Set {D} of held normal snags to 0,", "add the {D} value to their {H}" } },
    /*  38 AirSupport */
    { EventKey::Control, 7,  230,  232,
      "Air Support",
      { "Halve {H} of held normal snags.", "" } },
    /*  39 ALongRoad */
    { EventKey::Health, 5,  818,   58,
      "A Long Road",
      { "Double the value of held {A} {H} {D}", "tiles which have 2 exits." } },
    /*  40 HiddenWealth */
    { EventKey::Experience, 5,  132,  290,
      "Hidden Wealth",
      { "Convert random tiles to Wealth.", "Wealth gives {C} and reduces {A}" } },
    /*  41 HypnoticGaze */
    { EventKey::Defence, 4,  230,    0,
      "Hypnotic Gaze",
      { "Move the nearest placed snag", "to the start of the trail." } },
    /*  42 Concentration */
    { EventKey::Health, 5,  622,  174,
      "Concentration",
      { "Halve the {D} of a held", "normal or special snag." } },
    /*  43 GlaringHeadlights */
    { EventKey::Attack, 5,  426,  290,
      "Glaring Headlights",
      { "Gain 2 {A} per placed {X} tile.", "" } },
    /*  44 Flying */
    { EventKey::Experience, 3,  916,  174,
      "Flying",
      { "Discard all non-snag held tiles.", "Gain the value of {A} {H} {D} tiles." } },
    /*  45 UnlockedDoor */
    { EventKey::Experience, 6,  328,  290,
      "Unlocked Door",
      { "Reduce the number of keys", "required to unlock exit by 1." } },
    /*  46 ConsolidatePower */
    { EventKey::Attack, 4,  622,  232,
      "Consolidate Power",
      { "Combine all held {A} into one tile.", "" } },
    /*  47 StrongMedicine */
    { EventKey::Defence, 7,  426,  232,
      "Strong Medicine",
      { "Convert selected tiles to blanks.", "Gain 3 {D} per held blank." } },
    /*  48 Premonition */
    { EventKey::Control, 5,  524,  290,
      "Premonition",
      { "Discard a selected tile.", "Draw a snag with 1 {A}" } },
    /*  49 WantonDestruction */
    { EventKey::Defence, 4,  328,  232,
      "Wanton Destruction",
      { "Discard 2 random {D} tiles.", "Draw a snag with 1 {H}" } },
    /*  50 OldFaith */
    { EventKey::Attack, 4,  426,  348,
      "Old Faith",
      { "Draw one Faith tile per held {H}", "Faith gives {A} when placed." } },
    /*  51 LongRangeScan */
    { EventKey::Health, 3,  818,  348,
      "Long-Range Scan",
      { "Draw a Foresight tile.", "Foresight blocks one snag attack." } },
    /*  52 Rewind */
    { EventKey::Control, 4,  720,  348,
      "Rewind",
      { "Discard the leftmost held tile.", "Affects special snags." } },
    /*  53 Overcharge */
    { EventKey::Experience, 4,  328,  464,
      "Overcharge",
      { "Double value of a selected {C} tile.", "" } },
    /*  54 WellOiledMachine */
    { EventKey::Health, 4,  524,  348,
      "Well-Oiled Machine",
      { "Divide {H} of held normal snags", "by the number of held {C} tiles." } },
    /*  55 SweetMemento */
    { EventKey::Experience, 4,  328,  406,
      "Sweet Memento",
      { "Convert {H} tiles to Mementos.", "Mementos give {A} {H} {D}" } },
    /*  56 WarmGlow */
    { EventKey::Control, 6,  524,  464,
      "Warm Glow",
      { "Draw a Warmth tile.", "Warmth halves placed snags' {H}" } },
    /*  57 Sideswipe */
    { EventKey::Defence, 3,  916,  290,
      "Sideswipe",
      { "Halve {A} of a normal held snag.", "" } },
    /*  58 StrongVitals */
    { EventKey::Defence, 5,  230,  406,
      "Strong Vitals",
      { "Gain {H} equal to Nemesis XP.", "Gain {D} equal to Nemesis level." } },
    /*  59 Stampede */
    { EventKey::Attack, 6,  524,  406,
      "Stampede",
      { "Advance Nemesis to next snag.", "Set that snag's {D} to 0." } },
    /*  60 LuckyDraw */
    { EventKey::Health, 6,  720,  290,
      "Lucky Draw",
      { "Creates Good Luck tokens at", "exits of current tile, these give {X}" } },
    /*  61 HardWork */
    { EventKey::Experience, 5,  426,  406,
      "Hard Work",
      { "Draw an Effort tile.", "Effort grows its {A} {D} value." } },
    /*  62 FlameBreath */
    { EventKey::Control, 5,  328,  348,
      "Flame Breath",
      { "Gain {A} {D} equal to your {C}", "multiplied by your event count." } },
    /*  63 Snapshot */
    { EventKey::Defence, 6,  818,  464,
      "Snapshot",
      { "Gain 1 {X} immediately.", "" } },
    /*  64 AmmoDump */
    { EventKey::Attack, 6,  132,  406,
      "Ammo Dump",
      { "Convert all held {A} tiles", "to value 2 {C} tiles." } },
    /*  65 Backtrack */
    { EventKey::Health, 5,  720,  464,
      "Backtrack",
      { "Convert a held tile to Milestone,", "which gives {H} and moves a snag." } },
    /*  66 Disarm */
    { EventKey::Attack, 6,  230,  348,
      "Disarm",
      { "Create Weakness tokens around", "current tile, these halve snag {A}" } },
    /*  67 SafetyInNumbers */
    { EventKey::Control, 4,  622,  464,
      "Safety in Numbers",
      { "Combine held normal snags, draw", "{H} tiles to replace combined tiles." } },
    /*  68 Breakthrough */
    { EventKey::Experience, 4,  916,  406,
      "Breakthrough",
      { "Discard a held normal snag.", "Gain half its {D}" } },
    /*  69 FearlessDefence */
    { EventKey::Defence, 6,  818,  290,
      "Fearless Defence",
      { "Discard a random tile, draw {D}", "valued at number of held exits." } },
    /*  70 Clocktower */
    { EventKey::Experience, 4,  230,  464,
      "Clocktower",
      { "Creates Talent tokens around the", "current tile, these double {A} {H} {D}" } },
    /*  71 IdyllicLandscape */
    { EventKey::Control, 6,  132,  348,
      "Idyllic Landscape",
      { "Move all placed snags to", "the start of the trail." } },
    /*  72 InnerStrength */
    { EventKey::Health, 6,  818,  406,
      "Inner Strength",
      { "Gain {A} equal to half your {D}", "Gain {D} equal to half your {A}" } },
    /*  73 PuppetMaster */
    { EventKey::Experience, 4,  720,  406,
      "Puppet Master",
      { "Gain 1 {C} per 2 held snags.", "" } },
    /*  74 EmergencyBroadcast */
    { EventKey::Attack, 3,  426,  464,
      "Emergency Broadcast",
      { "Discard 2 random tiles.", "Draw 2 {A} tiles." } },
    /*  75 Countdown */
    { EventKey::Attack, 5,  622,  348,
      "Countdown",
      { "Draw an Honesty tile.", "Honesty gives {A}" } },
    /*  76 CovertStrike */
    { EventKey::Defence, 5,  622,  290,
      "Covert Strike",
      { "Defeat the nearest placed", "normal snag. Gain 1 {X}" } },
    /*  77 SapphireCity */
    { EventKey::Defence, 5,  132,  464,
      "Sapphire City",
      { "Gain 1 {D} per 3 placed tiles.", "" } },
    /*  78 CostlyGift */
    { EventKey::Health, 3,  916,  348,
      "Costly Gift",
      { "Discard 1-5 random tiles.", "Gain 1 {X}" } },
    /*  79 PureEfficiency */
    { EventKey::Control, 4,  622,  406,
      "Pure Efficiency",
      { "Discard all held {C} tiles.", "Gain 1 {C} per 2 tiles discarded." } },
    /*  80 Exhaustion */
    { EventKey::Attack, 6,  916,  464,
      "Exhaustion",
      { "Discard a random non-snag tile.", "" } },
};
