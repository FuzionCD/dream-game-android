#pragma once

// item-side static data tables. one stop for everything `Item::init`
// reads while building a fresh Item: SpecialAbility pool (this file),
// per-subType cosmetic name pool, and per-Item-type icon UVs (to be
// added as those binary tables get extracted).
//
// ------------------------------------------------------------
// 1. SPECIAL ABILITY POOL (DAT_100079da0)
// ------------------------------------------------------------
// static-init table read by FUN_10003040c (Item::init) when rolling a new
// Item's SpecialAbility slots.
//
// pool layout: 18 entries, each 16 bytes:
//   +0x00: int lo
//   +0x04: int hi_step
//   +0x08: const char* name (with {A}/{D}/{H}/{C}/{X} icon placeholders)
//
// magnitude rolled per slot:
//   magnitude = rngInt(lo, lo + (PlayerSystem.currentLevel / 10) * hi_step)
//
// each Item type draws from a 6-entry SUB-POOL:
//   type 0 (ATK items) -> indices 1..6
//   type 1 (HP items)  -> indices 7..12
//   type 2 (DEF items) -> indices 13..18
//
// entry index 0 (sentinel) is unused; the binary skips it.
//
// pattern matches perk_table.h: header-only inline constexpr.

struct SpecialAbilityEntry {
    int         lo;
    int         hiStep;
    const char* name;
};

// 19 entries (index 0 unused, 1..18 real). 1-indexed to match the binary.
inline constexpr SpecialAbilityEntry SPECIAL_ABILITY_POOL[19] = {
    // index 0: sentinel, not used by the binary.
    { 0, 0, nullptr },

    { 1, 1, "+%d {A} per discarded {A}" },          // 1
    { 1, 1, "-%d {A} lost on attack" },             // 2
    { 1, 1, "+%d {A} per placed {D}" },             // 3
    { 1, 1, "+%d {A} to left {A} per turn" },       // 4
    { 1, 1, "-%d {H} held snags per turn" },        // 5
    { 1, 0, "+%d {C} per level up" },               // 6
    { 1, 1, "+%d {H} every other turn" },           // 7
    { 1, 1, "+%d {H} per discarded {H}" },          // 8
    { 2, 1, "+%d snag {A} loss on attack" },        // 9
    { 2, 1, "+%d snag {D} loss on attack" },        // 10
    { 2, 2, "+%d {H} per placed {C}" },             // 11
    { 1, 1, "+%d {H} to a held {H} per turn" },     // 12
    { 1, 1, "+%d {D} per discarded {D}" },          // 13
    { 1, 1, "-%d {D} lost on attack" },             // 14
    { 2, 2, "+%d {D} per placed snag" },            // 15
    { 1, 1, "+%d {D} to right {D} per turn" },      // 16
    { 2, 2, "-%d {H} placed snags per turn" },      // 17
    { 1, 0, "+%d {X} per item upgrade" },           // 18
};

// per-Item-type sub-pool ranges. used by FUN_10003040c to clamp the
// abilityType roll to the type's 6 valid entries.
struct SpecialAbilitySubPool {
    int firstIdx;
    int count;        // always 6 in the binary
};

inline constexpr SpecialAbilitySubPool SPECIAL_ABILITY_SUBPOOL[3] = {
    { 1,  6 },   // type 0 (ATK)
    { 7,  6 },   // type 1 (HP)
    { 13, 6 },   // type 2 (DEF)
};

constexpr int SPECIAL_ABILITY_COUNT = 18;

// ------------------------------------------------------------
// 2. SUBTYPE COSMETIC NAME POOL (DAT_10007e220)
// ------------------------------------------------------------
// the binary's FUN_100030a4c lazy-inits a per-type vector-of-vectors of
// cosmetic name strings at startup. flattened here at compile time:
//   COSMETIC_NAMES[type][subType] = CosmeticNameRow{ names[5], count }
//
// Item::init rolls subType in [0..55] for type 0/1 or [0..44] for type 2,
// then rolls cosmeticNameIdx within that subType's row. Item::getName
// returns names[cosmeticNameIdx], or "Strange Object" when count == 0.
// (no entry in the binary actually hits that fallback, but the binary
// reserves it.)

struct CosmeticNameRow {
    const char* names[5];   // up to 5 cosmetic names, trailing nullptr padded
    int         count;      // number of valid names (1..5)
};

// type 0 (ATK), 56 subTypes
inline constexpr CosmeticNameRow COSMETIC_NAMES_TYPE0[56] = {
        { { "Machine Gun", "Toy Gun", "Airsoft Carbine", "Submachine Gun", "Pellet Gun" }, 5 },  // subType 0x00
        { { "Handheld Radio", "Noisy Radio", "Walkie-talkie", "CB Radio", "Police Radio" }, 5 },  // subType 0x01
        { { "Rubber Knife", "Electric Knife", "Combat Knife", "Serrated Knife", "Stabber" }, 5 },  // subType 0x02
        { { "Brass Knuckles", "Knuckledusters", "Stained Knuckles", "Spiked Knuckles", "Glass Knuckles" }, 5 },  // subType 0x03
        { { "Empty Mug", "Cup of Coffee", "Cup of Tea", "Coffee Mug", "White Mug" }, 5 },  // subType 0x04
        { { "Baseball Bat", "Wooden Bat", "Autographed Bat", "Large Club", "Wooden Club" }, 5 },  // subType 0x05
        { { "Grenade", "Fake Grenade", "Dud Grenade", "Frag Grenade", "Hidden Grenade" }, 5 },  // subType 0x06
        { { "Flintlock Pistol", "Antique Gun", "Replica Pistol", "Masterwork Pistol", "Crude Pistol" }, 5 },  // subType 0x07
        { { "Obsidian Skull", "Monkey Skull", "Ominous Stone", "Glittering Skull", "Poor Yorick" }, 5 },  // subType 0x08
        { { "Lighter", "Eternal Lighter", "Missing Lighter", "Heirloom Lighter", "Defective Lighter" }, 5 },  // subType 0x09
        { { "Crowbar", "New Crowbar", "Magnetic Crowbar", "Red Crowbar", "Prybar" }, 5 },  // subType 0x0a
        { { "Guitar", "Bass", "Electric Guitar", "Rock Guitar", "Lead Guitar" }, 5 },  // subType 0x0b
        { { "Unreadable Notes", "Homework", "Unfinished Essay", "Old Homework", "Odd Note" }, 5 },  // subType 0x0c
        { { "Megaphone", "Scratchy Megaphone", "Bullhorn", "Silent Bullhorn", "Black Megaphone" }, 5 },  // subType 0x0d
        { { "Spork", "Plastic Spork", "Golden Spork", "Fragile Spork", "Tiny Spork" }, 5 },  // subType 0x0e
        { { "Timekeeper", "Inscrutable Watch", "Stopped Watch", "Digital Watch", "Expensive Watch" }, 5 },  // subType 0x0f
        { { "Hatchet", "Woodcutter's Axe", "Chopper", "Dull Axe", "Keen Axe" }, 5 },  // subType 0x10
        { { "Bottle of Water", "Plastic Bottle", "Soft Drink", "Bottled Tea", "Empty Bottle" }, 5 },  // subType 0x11
        { { "Phone", "Ringing Phone", "Dropped Phone", "Glass Phone", "Broken Phone" }, 5 },  // subType 0x12
        { { "Tasty Sandwich", "Moldy Sandwich", "Egg Sandwich", "Fresh Sandwich", "Fruit Sandwich" }, 5 },  // subType 0x13
        { { "Coffee Cup", "Paper Cup", "Misplaced Cup", "Strong Coffee", "Hot Chocolate" }, 5 },  // subType 0x14
        { { "Camera", "3D Camera", "Waterproof Camera", "Video Camera", "Hidden Camera" }, 5 },  // subType 0x15
        { { "Laptop", "Broken Laptop", "Beeping Laptop", "New Computer", "Borrowed Laptop" }, 5 },  // subType 0x16
        { { "Magnifying Glass", "Hand Lens", "Warped Lens", "Magnifier", "Magnifying Lens" }, 5 },  // subType 0x17
        { { "Whistle", "Loud Whistle", "Subsonic Whistle", "Silent Whistle", "Melodic Whistle" }, 5 },  // subType 0x18
        { { "Sabre", "Cavalry Sabre", "Rattling Sabre", "Glowing Falchion", "Double-edged Sword" }, 5 },  // subType 0x19
        { { "Flashlight", "Metal Bar", "Flickery Flashlight", "Dim Torch", "Bright Flashlight" }, 5 },  // subType 0x1a
        { { "Skull Ring", "Titanium Ring", "Loose Ring", "Ruby Ring", "Spiky Ring" }, 5 },  // subType 0x1b
        { { "Binoculars", "Reverse Binoculars", "Magic Binoculars", "Tactical Binoculars", "Night Vision Goggles" }, 5 },  // subType 0x1c
        { { "Textbook", "Leatherbound Tome", "Photo Book", "Mysterious Novel", "Book of Omens" }, 5 },  // subType 0x1d
        { { "Revolver", "Jammed Pistol", "Unloaded Gun", "Prop Gun", "Snubnosed Revolver" }, 5 },  // subType 0x1e
        { { "Briefcase", "Leather Briefcase", "Unexpected Briefcase", "Heavy Luggage", "Locked Case" }, 5 },  // subType 0x1f
        { { "Curious Device", "Box of Electronics", "Glowing Device", "Faulty Gadget", "Complex Device" }, 5 },  // subType 0x20
        { { "Mallet", "Sledgehammer", "Wooden Hammer", "Rubber Mallet", "Gavel" }, 5 },  // subType 0x21
        { { "Pink Bracelet", "Rubber Bracelet", "Plain Ring", "Wedding Ring", "Ring of Binding" }, 5 },  // subType 0x22
        { { "Voodoo Doll", "Old Toy", "Stuffed Doll", "Paper Doll", "Plush Toy" }, 5 },  // subType 0x23
        { { "Important Documents", "Secret Files", "Stolen Files", "Lost Documents", "Signed Documents" }, 5 },  // subType 0x24
        { { "Lucky Coin", "Gold Dubloon", "One-sided Coin", "Shiny Coin", "Buried Coin" }, 5 },  // subType 0x25
        { { "Fountain Pen", "Glitter Pen", "Gift Pen", "Red Pen", "Expensive Pen" }, 5 },  // subType 0x26
        { { "Empty Hands", "Rubber Gloves", "Warm Gloves", "Bare Hands", "Dirty Hands" }, 5 },  // subType 0x27
        { { "Ace of Spades", "Winning Card", "Ace in the Hole", "Lucky Card", "Playing Card" }, 5 },  // subType 0x28
        { { "Sketchpad", "Sketchbook", "Last Sketch", "Rough Sketch", "Notepad" }, 5 },  // subType 0x29
        { { "Yesterday's Newspaper", "Soggy Newspaper", "Scrambled Newsprint", "Tabloid", "Old Newspaper" }, 5 },  // subType 0x2a
        { { "Pliers", "Blacksilver Pliers", "Pincers", "Rusty Pliers", "Greasy Pliers" }, 5 },  // subType 0x2b
        { { "Tarot Card", "Inkblot", "Trading Card", "Blurry Photo", "Signed Photo" }, 5 },  // subType 0x2c
        { { "Candlestick", "Table Leg", "Silver Candlestick", "Ornate Candlestick", "Heavy Candlestick" }, 5 },  // subType 0x2d
        { { "Scissors", "Shears", "Blunt Scissors", "Miniature Scissors", "Safety Scissors" }, 5 },  // subType 0x2e
        { { "Tape Recorder", "Cassette Player", "Tape Deck", "Voice Recorder", "Unusual Device" }, 5 },  // subType 0x2f
        { { "Coffee Pot", "Pot of Coffee", "Empty Pot", "Cracked Pot", "Water Jug" }, 5 },  // subType 0x30
        { { "Truncheon", "Baton", "Crimson Club", "Blackjack", "Steel Club" }, 5 },  // subType 0x31
        { { "Glass Vial", "Viscous Potion", "Empty Phial", "Sealed Vial", "Strong Poison" }, 5 },  // subType 0x32
        { { "Green Apple", "Crunchy Apple", "Rotten Apple", "Frozen Apple", "Baked Apple" }, 5 },  // subType 0x33
        { { "Faberge Egg", "Legendary Relic", "Ancient Artifact", "Plastic Egg", "Jewelled Egg" }, 5 },  // subType 0x34
        { { "Ritual Dagger", "Vorpal Knife", "Rusty Dagger", "Jewelled Dagger", "Piercing Dagger" }, 5 },  // subType 0x35
        { { "Ice Cream", "Chocolate Ice Cream", "Vanilla Soft Serve", "Melting Ice Cream", "Frozen Yogurt" }, 5 },  // subType 0x36
        { { "The Key", "Brass Key", "Ephemeral Key", "Special Key", "False Key" }, 5 },  // subType 0x37
};

// type 1 (HP), 56 subTypes
inline constexpr CosmeticNameRow COSMETIC_NAMES_TYPE1[56] = {
        { { "Police Hat", "Peaked Cap", "Uniform Cap", "Navy Cap", "Officer Cap" }, 5 },  // subType 0x00
        { { "Band-aids", "Healing Bandages", "Potent Band-aids", "Luminescent Bandages", "Sticky Band-aids" }, 5 },  // subType 0x01
        { { "Cherry Lollipop", "Blue Lollipop", "Cola Lollipop", "Lollipop", "Sour Lollipop" }, 5 },  // subType 0x02
        { { "Luchador Mask", "Wrestling Mask", "Mask of Fear", "Ousted Mask", "Discarded Mask" }, 5 },  // subType 0x03
        { { "Monocle", "Ring Necklace", "Ring on a String", "Gold Monocle", "Fetching Monocle" }, 5 },  // subType 0x04
        { { "Flat Cap", "Army Cap", "Loose Cap", "Khaki Cap", "Dusty Cap" }, 5 },  // subType 0x05
        { { "Red Beret", "Black Beret", "Commando Beret", "Beret", "Guerilla Beret" }, 5 },  // subType 0x06
        { { "Motorbike Helmet", "Racing Helmet", "Padded Helmet", "Comfortable Helmet", "Rally Helmet" }, 5 },  // subType 0x07
        { { "Skull Helmet", "Dire Helmet", "Morbid Helm", "Grim Helmet", "Void Helm" }, 5 },  // subType 0x08
        { { "Goggles", "Science Goggles", "Safety Goggles", "Safety Glasses", "Ski Goggles" }, 5 },  // subType 0x09
        { { "Birthday Hat", "Party Hat", "Fun Hat", "Dunce Cap", "Cone Cap" }, 5 },  // subType 0x0a
        { { "Party Horn", "Paper Horn", "Noisemaker", "Tweeter", "Feathered Noisemaker" }, 5 },  // subType 0x0b
        { { "Winged Helmet", "Mercury's Helm", "Iridescent Helm", "Talking Helmet", "Helm of Freedom" }, 5 },  // subType 0x0c
        { { "Bandana", "Headband", "Cool Headband", "Golden Headband", "Checkered Headband" }, 5 },  // subType 0x0d
        { { "Eyepatch", "Metal Eyepatch", "Pirate Eyepatch", "Custom Eyepatch", "White Eyepatch" }, 5 },  // subType 0x0e
        { { "3D Glasses", "Stereo Glasses", "Paper Glasses", "Red-blue Glasses", "Magic Glasses" }, 5 },  // subType 0x0f
        { { "Top Hat", "Extravagant Hat", "Magic Hat", "Formal Hat", "White Top Hat" }, 5 },  // subType 0x10
        { { "Flu Mask", "Face Mask", "Surgical Mask", "Gauze Mask", "Sterile Mask" }, 5 },  // subType 0x11
        { { "Gas Mask", "Combat Helmet", "Tactical Helmet", "Timely Gas Mask", "Stifling Gas Mask" }, 5 },  // subType 0x12
        { { "Ankh Earring", "Phylactery", "Ankh Tattoo", "Alluring Earring", "Talisman of Life" }, 5 },  // subType 0x13
        { { "Santa Hat", "Warm Hat", "Floppy Hat", "Fur Hat", "Blue Fur Hat" }, 5 },  // subType 0x14
        { { "Glasses", "Trendy Glasses", "Black Sunglasses", "X-ray Glasses", "Infrared Glasses" }, 5 },  // subType 0x15
        { { "Mohawk", "Radiant Mohawk", "Mystic Mohawk", "Tribal Mohawk", "Unkempt Mohawk" }, 5 },  // subType 0x16
        { { "Hockey Mask", "Sinister Mask", "Evil Mask", "Porcelain Mask", "Mask of Oppression" }, 5 },  // subType 0x17
        { { "Cavalry Hat", "Cavalry Stetson", "Felt Hat", "Elegant Hat", "Sturdy Hat" }, 5 },  // subType 0x18
        { { "Earbuds", "Earphones", "Crackly Earphones", "Unplugged Earphones", "New Earbuds" }, 5 },  // subType 0x19
        { { "Spiked Helmet", "Gladiator Helm", "Spiky Hat", "Steel Mohawk", "Helm of Thorns" }, 5 },  // subType 0x1a
        { { "Sheriff Hat", "Cowboy Hat", "Marshal's Hat", "Stout Hat", "Leather Hat" }, 5 },  // subType 0x1b
        { { "Swimming Goggles", "Goggles", "Tinted Goggles", "Driving Goggles", "Black Goggles" }, 5 },  // subType 0x1c
        { { "Prescription Glasses", "Vanity Glasses", "Purple Glasses", "Reading Glasses", "Lost Glasses" }, 5 },  // subType 0x1d
        { { "Hard Hat", "Safety Helmet", "Orange Helmet", "Aluminium Helmet", "Bulletproof Helmet" }, 5 },  // subType 0x1e
        { { "Third Eye", "Precious Heirloom", "Red Eye Ruby", "Electronic Eye", "Third Eye Tattoo" }, 5 },  // subType 0x1f
        { { "Shutter Shades", "Conspicuous Shades", "Blue Shutter Shades", "Venetian Blinders", "Metal Shades" }, 5 },  // subType 0x20
        { { "Bandana", "Skullcap", "Kerchief", "Outlaw Headwrap", "Patterned Bandana" }, 5 },  // subType 0x21
        { { "Cardboard Mask", "Masquerade Mask", "Painted Mask", "Glittering Mask", "Mask of Confidence" }, 5 },  // subType 0x22
        { { "Empty Pipe", "Strange Pipe", "Brass Pipe", "Bone Pipe", "Clay Pipe" }, 5 },  // subType 0x23
        { { "Headset", "Headphones", "Muffled Headphones", "Silent Headset", "Wireless Headphones" }, 5 },  // subType 0x24
        { { "Bowler Hat", "Brown Bowler Hat", "Cheap Bowler Hat", "Tight Hat", "Unpleasant Hat" }, 5 },  // subType 0x25
        { { "Baseball Cap", "Backwards Cap", "Colourful Cap", "Autographed Cap", "Borrowed Cap" }, 5 },  // subType 0x26
        { { "Aviators", "Mirrored Glasses", "Sunglasses", "Polarised Sunglasses", "Cracked Glasses" }, 5 },  // subType 0x27
        { { "Spade Necklace", "Stylish Necklace", "Amulet of Luck", "Silver Necklace", "Rare Necklace" }, 5 },  // subType 0x28
        { { "Scarf", "Torn Scarf", "Xanthous Scarf", "Soft Scarf", "Knitted Scarf" }, 5 },  // subType 0x29
        { { "Bow Tie", "Chrome Bow Tie", "Rainbow Tie", "Clip-on Bow Tie", "Loose Bow Tie" }, 5 },  // subType 0x2a
        { { "Ankh Necklace", "Chain Necklace", "Amulet of Protection", "Glowing Amulet", "Solemn Necklace" }, 5 },  // subType 0x2b
        { { "Dogtags", "Dogtag Necklace", "Mismatched Dogtags", "Blank Dogtags", "Charred Dogtags" }, 5 },  // subType 0x2c
        { { "Onyx Crown", "Gold Crown", "Shattered Crown", "Heavy Crown", "Crown of Bone" }, 5 },  // subType 0x2d
        { { "Iron Mask", "Robot Helmet", "Steel Helm", "Cast Iron Helm", "Forged Helmet" }, 5 },  // subType 0x2e
        { { "Fancy Wig", "Dazzling Wig", "Orange Wig", "Neon Wig", "Itchy Wig" }, 5 },  // subType 0x2f
        { { "Roman Helm", "Centurion Helm", "Legion Helmet", "Plastic Helmet", "Galea" }, 5 },  // subType 0x30
        { { "Pirate Hat", "Pirate Tricorn", "Privateer Hat", "Feathered Tricorn", "Sea-soaked Tricorn" }, 5 },  // subType 0x31
        { { "Bunny Ears", "Rabbit Ears", "Fluffy Ears", "Pink Bunny Ears", "Rabbit Ear Headband" }, 5 },  // subType 0x32
        { { "Rabbit Mask", "Chilling Mask", "Beautiful Mask", "Jackalope Mask", "Mask of Denial" }, 5 },  // subType 0x33
        { { "Wizard Hat", "Magic Hat", "Tinfoil Hat", "Pointy Hat", "Witch's Hat" }, 5 },  // subType 0x34
        { { "Tiara", "Ornate Tiara", "Bronze Tiara", "Paper Tiara", "Forgotten Coronet" }, 5 },  // subType 0x35
        { { "Graduation Cap", "Mortarboard", "Used Mortarboard", "Gold-foiled Mortarboard", "Transcendent Cap" }, 5 },  // subType 0x36
        { { "Round Glasses", "Bloodstained Glasses", "Rose-tinted Glasses", "Fragile Glasses", "Glasses of Hope" }, 5 },  // subType 0x37
};

// type 2 (DEF), 45 subTypes
inline constexpr CosmeticNameRow COSMETIC_NAMES_TYPE2[45] = {
        { { "Hoodie", "Stained Hoodie", "Soft Hoodie", "Baggy Hoodie", "Stealthy Hoodie" }, 5 },  // subType 0x00
        { { "Satchel", "Bag of Holding", "Handbag", "Leather Bag", "Full Bag" }, 5 },  // subType 0x01
        { { "Hooded Cloak", "Dark Cloak", "Cowled Cloak", "Cloak of Invisibility", "Wool Cloak" }, 5 },  // subType 0x02
        { { "T-shirt", "Yellow Tee", "Plain T-shirt", "Tight T-shirt", "Baggy T-shirt" }, 5 },  // subType 0x03
        { { "Boutonniere", "Lapel Pin", "Lapel Flower", "Desert Flower", "Fragrant Flower" }, 5 },  // subType 0x04
        { { "Skull Belt", "Power Belt", "Tight Belt", "Twisted Belt", "Auspicious Belt" }, 5 },  // subType 0x05
        { { "Raincoat", "Yellow Raincoat", "Flimsy Raincoat", "Mystic Raincoat", "Windbreaker" }, 5 },  // subType 0x06
        { { "Leather Belt", "White Leather Belt", "Belt of Stability", "Magic Belt", "Belt of Illusions" }, 5 },  // subType 0x07
        { { "Backpack", "School Backpack", "Camping Backpack", "Travel Backpack", "Empty Backpack" }, 5 },  // subType 0x08
        { { "Padded Jacket", "Winter Jacket", "Quilted Jacket", "Cozy Jacket", "Sweltering Jacket" }, 5 },  // subType 0x09
        { { "Track Jacket", "Windproof Jacket", "Running Jacket", "Loose Jacket", "Floral Jacket" }, 5 },  // subType 0x0a
        { { "Lab Coat", "Starched Coat", "Pinstriped Coat", "Long Coat", "Plastic Coat" }, 5 },  // subType 0x0b
        { { "Motorcycle Jacket", "Racing Suit", "Leather Jacket", "Designer Jacket", "Yellow Leather Jacket" }, 5 },  // subType 0x0c
        { { "Cardigan", "Knitted Cardigan", "Striped Cardigan", "Unusual Cardigan", "Ragged Cardigan" }, 5 },  // subType 0x0d
        { { "Fire Jacket", "Flameproof Suit", "Reflective Jacket", "Dazzling Jacket", "Fire Suit" }, 5 },  // subType 0x0e
        { { "Medal", "Medal of Liberty", "Antique Medal", "Hidden Medal", "Coveted Award" }, 5 },  // subType 0x0f
        { { "Studded Jacket", "Snazzy Jacket", "Thick Leather Jacket", "Biker Jacket", "Tight Leather Jacket" }, 5 },  // subType 0x10
        { { "Flame Shirt", "Garish Shirt", "Patterned Shirt", "Bowling Shirt", "Jazzy Shirt" }, 5 },  // subType 0x11
        { { "First Aid Kit", "Medkit", "Medicine Bag", "Bag of Healing", "Trauma Kit" }, 5 },  // subType 0x12
        { { "Fancy Coat", "Pirate Coat", "Stylish Coat", "Frilly Coat", "Lavish Coat" }, 5 },  // subType 0x13
        { { "Bulletproof Vest", "Kevlar Vest", "Reinforced Vest", "Ballistic Vest", "Flak Jacket" }, 5 },  // subType 0x14
        { { "Pouch Belt", "Utility Belt", "Tool Belt", "Hunting Belt", "Tactical Belt" }, 5 },  // subType 0x15
        { { "Reflective Vest", "Safety Vest", "Luminous Vest", "Radiant Vest", "Neon Vest" }, 5 },  // subType 0x16
        { { "Pieces of Flair", "Mixed Badges", "Colourful Pins", "Edgy Pins", "Limited Edition Badges" }, 5 },  // subType 0x17
        { { "Bandolier", "Ammo Belt", "Shotgun Sling", "Stylish Bandolier", "Spent Ammo Belt" }, 5 },  // subType 0x18
        { { "Trenchcoat", "Heavy Trenchcoat", "Detective's Coat", "Drab Trenchcoat", "Ragged Trenchcoat" }, 5 },  // subType 0x19
        { { "Denim Jacket", "Checkered Shirt", "Flannel Shirt", "Camo Jacket", "Old Jacket" }, 5 },  // subType 0x1a
        { { "Warm Cloak", "Cape", "Leather Cape", "Black Cape", "Flowing Cape" }, 5 },  // subType 0x1b
        { { "Turtleneck", "Snug Turtleneck", "Mottled Turtleneck", "Mock Turtleneck", "Silk Turtleneck" }, 5 },  // subType 0x1c
        { { "Singlet", "Tank Top", "Sporty Singlet", "Bright Tank Top", "Tie-dye Singlet" }, 5 },  // subType 0x1d
        { { "Skull Patch", "Skull Tattoo", "Morbid Patch", "Iron-on Patch", "Eerie Tattoo" }, 5 },  // subType 0x1e
        { { "Bloodied Shirt", "Dirty Shirt", "Grimy Shirt", "Torn Shirt", "Crumpled Shirt" }, 5 },  // subType 0x1f
        { { "Studded Belt", "Spiked Belt", "Jewelled Belt", "Belt of Immunity", "Arcane Belt" }, 5 },  // subType 0x20
        { { "Heavy Armour", "Space Suit", "Powered Armour", "Kevlar Suit", "Bulletproof Armour" }, 5 },  // subType 0x21
        { { "Leather Vest", "Biker Vest", "Studded Vest", "Black Leather Vest", "Thick Leather Vest" }, 5 },  // subType 0x22
        { { "Gun Holster", "Empty Holster", "Pistol Holster", "Leather Holster", "Holster Belt" }, 5 },  // subType 0x23
        { { "Itchy Sweater", "Christmas Sweater", "Wool Sweater", "Warm Jumper", "Blue Jumper" }, 5 },  // subType 0x24
        { { "Police Shirt", "Sheriff Shirt", "Uniform Shirt", "Officer Shirt", "Navy Shirt" }, 5 },  // subType 0x25
        { { "Combat Vest", "Army Vest", "Tactical Vest", "Khaki Vest", "Utility Vest" }, 5 },  // subType 0x26
        { { "Steel Breastplate", "Cast Iron Breastplate", "Reinforced Armour", "Chrome Breastplate", nullptr }, 4 },  // subType 0x27
        { { "Nametag", "Illegible Nametag", "Blurry Nametag", "Wrong Nametag", "Eye-catching Nametag" }, 5 },  // subType 0x28
        { { "Jetpack", "Rocket Pack", "Booster Pack", "Jump Pack", "Rocket Jetpack" }, 5 },  // subType 0x29
        { { "Plate Armour", "Splint Mail", "Shadow Armour", "Armour of Faith", "Rusty Armour" }, 5 },  // subType 0x2a
        { { "Resplendent Wings", "Stained Glass Wings", "Gargoyle Wings", "Angel Wings", "Glorious Wings" }, 5 },  // subType 0x2b
        { { "Hula Hoop", "Rainbow Hula Hoop", "Spiral Hoop", "Glittering Hula Hoop", "Neon Hoop" }, 5 },  // subType 0x2c
};

// ------------------------------------------------------------
// 3. PER-ITEM-TYPE ICON UVs (DAT_100079ed0 / DAT_10007a250 / DAT_10007a5d0)
// ------------------------------------------------------------
// FUN_100032a74 sets up each Item's 4 icon TileIcons. warnLine1 (the base
// item silhouette) comes from a per-type table indexed by FUN_10003326c.
// warnLine2/3/4 (the ATK/DEF/HP stat-number backgrounds) and the 3 ColorTints
// have shared UVs across all items.
//
// the SHARED values are inlined in item.cpp's postInitVisuals:
//   warnLine2 UV: (0.18164, 0.0)..+(0.075, 0.072)   ATK number background
//   icon3 UV: (0.28027, 0.0)..+(0.069, 0.072)   DEF number background
//   icon4 UV: (0.22949, 0.0)..+(0.078, 0.072)   HP number background
//   tint1 color: 0xFFC8FFFF (ATK number, yellow-tinted white)
//   tint2 color: 0xFFFFE6C8 (DEF number, off-white pink)
//   tint3 color: 0xFF64FFFF (HP number, cyan)
//   SpecialAbility iconQuad UV: (0.51855, 0.20019)..+0.0625x0.0625
//
// the PER-TYPE tables below hold the atlas-pixel (x, y, w, h) for warnLine1,
// fed to setPixelRect on warnLine1.quad in postInitVisuals.

struct IconRect {
    int atlasX;
    int atlasY;
    int atlasW;
    int atlasH;
};

// type 0 (ATK), 56 warnLine1 atlas rects (atlasX, atlasY, atlasW, atlasH)
inline constexpr IconRect ICON_RECT_TYPE0[56] = {
        {    0,    0,   78,   78 },  // subType 0x00
        {   79,    0,   69,   77 },  // subType 0x01
        {  149,    0,   79,   76 },  // subType 0x02
        {  229,    0,   77,   76 },  // subType 0x03
        {  307,    0,   78,   76 },  // subType 0x04
        {  386,    0,   77,   77 },  // subType 0x05
        {  464,    0,   75,   76 },  // subType 0x06
        {  540,    0,   75,   77 },  // subType 0x07
        {  616,    0,   77,   70 },  // subType 0x08
        {  694,    0,   76,   75 },  // subType 0x09
        {  771,    0,   78,   77 },  // subType 0x0a
        {  850,    0,   78,   77 },  // subType 0x0b
        {  929,    0,   75,   77 },  // subType 0x0c
        {    0,   79,   77,   77 },  // subType 0x0d
        {   78,   79,   76,   76 },  // subType 0x0e
        {  155,   77,   76,   78 },  // subType 0x0f
        {  232,   77,   78,   78 },  // subType 0x10
        {  311,   78,   77,   77 },  // subType 0x11
        {  389,   78,   75,   77 },  // subType 0x12
        {  465,   78,   77,   78 },  // subType 0x13
        {  543,   78,   76,   78 },  // subType 0x14
        {  620,   76,   76,   76 },  // subType 0x15
        {  697,   78,   77,   75 },  // subType 0x16
        {  775,   78,   74,   76 },  // subType 0x17
        {  850,   78,   77,   77 },  // subType 0x18
        {  928,   78,   77,   79 },  // subType 0x19
        {    0,  157,   78,   74 },  // subType 0x1a
        {   79,  156,   77,   72 },  // subType 0x1b
        {  157,  156,   77,   74 },  // subType 0x1c
        {  235,  156,   77,   70 },  // subType 0x1d
        {  313,  156,   75,   77 },  // subType 0x1e
        {  389,  157,   77,   73 },  // subType 0x1f
        {  467,  157,   76,   77 },  // subType 0x20
        {  544,  157,   77,   75 },  // subType 0x21
        {  622,  154,   76,   77 },  // subType 0x22
        {  699,  155,   76,   76 },  // subType 0x23
        {  776,  156,   78,   76 },  // subType 0x24
        {  855,  158,   77,   72 },  // subType 0x25
        {  933,  158,   77,   77 },  // subType 0x26
        {    0,  232,   75,   78 },  // subType 0x27
        {   76,  232,   72,   76 },  // subType 0x28
        {  149,  231,   75,   79 },  // subType 0x29
        {  225,  231,   75,   77 },  // subType 0x2a
        {  301,  234,   77,   75 },  // subType 0x2b
        {  379,  234,   73,   76 },  // subType 0x2c
        {  453,  235,   75,   78 },  // subType 0x2d
        {  529,  235,   77,   76 },  // subType 0x2e
        {  607,  233,   78,   77 },  // subType 0x2f
        {  686,  232,   74,   76 },  // subType 0x30
        {  761,  233,   75,   77 },  // subType 0x31
        {  837,  233,   71,   77 },  // subType 0x32
        {  909,  236,   72,   77 },  // subType 0x33
        {    0,  311,   72,   76 },  // subType 0x34
        {   73,  311,   70,   79 },  // subType 0x35
        {  144,  311,   74,   76 },  // subType 0x36
        {  219,  311,   75,   72 },  // subType 0x37
};

// type 1 (HP), 56 warnLine1 atlas rects (atlasX, atlasY, atlasW, atlasH)
inline constexpr IconRect ICON_RECT_TYPE1[56] = {
        {  295,  310,   77,   73 },  // subType 0x00
        {  373,  311,   74,   75 },  // subType 0x01
        {  448,  314,   73,   76 },  // subType 0x02
        {  522,  314,   70,   75 },  // subType 0x03
        {  593,  312,   76,   77 },  // subType 0x04
        {  670,  311,   73,   77 },  // subType 0x05
        {  744,  311,   77,   73 },  // subType 0x06
        {  822,  311,   75,   76 },  // subType 0x07
        {  898,  314,   76,   77 },  // subType 0x08
        {    0,  391,   77,   74 },  // subType 0x09
        {   78,  391,   67,   76 },  // subType 0x0a
        {  146,  389,   77,   73 },  // subType 0x0b
        {  224,  389,   77,   77 },  // subType 0x0c
        {  302,  387,   76,   76 },  // subType 0x0d
        {  379,  391,   76,   73 },  // subType 0x0e
        {  456,  391,   77,   71 },  // subType 0x0f
        {  534,  390,   75,   77 },  // subType 0x10
        {  610,  390,   76,   75 },  // subType 0x11
        {  687,  389,   72,   76 },  // subType 0x12
        {  760,  388,   67,   79 },  // subType 0x13
        {  828,  392,   75,   77 },  // subType 0x14
        {  904,  392,   77,   71 },  // subType 0x15
        {    0,  466,   77,   72 },  // subType 0x16
        {   78,  468,   71,   76 },  // subType 0x17
        {  150,  467,   75,   71 },  // subType 0x18
        {  226,  467,   77,   76 },  // subType 0x19
        {  304,  465,   75,   77 },  // subType 0x1a
        {  380,  465,   76,   75 },  // subType 0x1b
        {  457,  463,   75,   77 },  // subType 0x1c
        {  533,  468,   76,   70 },  // subType 0x1d
        {  610,  466,   76,   74 },  // subType 0x1e
        {  687,  468,   76,   68 },  // subType 0x1f
        {  764,  470,   77,   75 },  // subType 0x20
        {  842,  470,   75,   78 },  // subType 0x21
        {  918,  464,   77,   75 },  // subType 0x22
        {    0,  539,   75,   74 },  // subType 0x23
        {   76,  545,   76,   76 },  // subType 0x24
        {  153,  544,   78,   71 },  // subType 0x25
        {  232,  544,   78,   76 },  // subType 0x26
        {  311,  543,   77,   74 },  // subType 0x27
        {  389,  541,   76,   76 },  // subType 0x28
        {  466,  541,   72,   77 },  // subType 0x29
        {  539,  541,   76,   72 },  // subType 0x2a
        {  616,  541,   77,   76 },  // subType 0x2b
        {  694,  546,   77,   75 },  // subType 0x2c
        {  772,  549,   79,   79 },  // subType 0x2d
        {  852,  549,   73,   76 },  // subType 0x2e
        {  926,  540,   75,   75 },  // subType 0x2f
        {    0,  614,   70,   74 },  // subType 0x30
        {   71,  622,   76,   76 },  // subType 0x31
        {  148,  622,   72,   77 },  // subType 0x32
        {  221,  621,   74,   75 },  // subType 0x33
        {  296,  621,   73,   74 },  // subType 0x34
        {  370,  618,   76,   74 },  // subType 0x35
        {  447,  619,   74,   76 },  // subType 0x36
        {  522,  619,   77,   68 },  // subType 0x37
};

// type 2 (DEF), 45 warnLine1 atlas rects (atlasX, atlasY, atlasW, atlasH)
inline constexpr IconRect ICON_RECT_TYPE2[45] = {
        {  600,  618,   72,   76 },  // subType 0x00
        {  673,  622,   74,   74 },  // subType 0x01
        {  748,  629,   67,   76 },  // subType 0x02
        {  816,  629,   72,   76 },  // subType 0x03
        {  889,  626,   71,   78 },  // subType 0x04
        {    0,  699,   75,   73 },  // subType 0x05
        {   76,  699,   71,   76 },  // subType 0x06
        {  148,  700,   77,   76 },  // subType 0x07
        {  226,  697,   75,   76 },  // subType 0x08
        {  302,  696,   73,   76 },  // subType 0x09
        {  376,  696,   74,   76 },  // subType 0x0a
        {  451,  696,   73,   77 },  // subType 0x0b
        {  525,  695,   76,   76 },  // subType 0x0c
        {  602,  697,   75,   75 },  // subType 0x0d
        {  678,  706,   76,   76 },  // subType 0x0e
        {  755,  706,   75,   77 },  // subType 0x0f
        {  831,  706,   73,   76 },  // subType 0x10
        {  905,  705,   73,   76 },  // subType 0x11
        {    0,  773,   74,   77 },  // subType 0x12
        {   75,  776,   72,   76 },  // subType 0x13
        {  148,  777,   71,   76 },  // subType 0x14
        {  220,  777,   76,   73 },  // subType 0x15
        {  297,  774,   75,   76 },  // subType 0x16
        {  373,  773,   76,   69 },  // subType 0x17
        {  450,  774,   76,   77 },  // subType 0x18
        {  527,  772,   74,   76 },  // subType 0x19
        {  602,  773,   73,   78 },  // subType 0x1a
        {  676,  783,   68,   77 },  // subType 0x1b
        {  745,  784,   75,   74 },  // subType 0x1c
        {  821,  784,   70,   76 },  // subType 0x1d
        {  892,  783,   74,   77 },  // subType 0x1e
        {    0,  853,   75,   77 },  // subType 0x1f
        {   76,  854,   77,   74 },  // subType 0x20
        {  154,  854,   76,   75 },  // subType 0x21
        {  231,  851,   75,   76 },  // subType 0x22
        {  307,  851,   77,   74 },  // subType 0x23
        {  385,  852,   75,   76 },  // subType 0x24
        {  461,  852,   75,   76 },  // subType 0x25
        {  537,  852,   76,   76 },  // subType 0x26
        {  614,  861,   74,   76 },  // subType 0x27
        {  689,  861,   79,   71 },  // subType 0x28
        {  769,  861,   76,   73 },  // subType 0x29
        {  846,  861,   76,   75 },  // subType 0x2a
        {  923,  861,   76,   73 },  // subType 0x2b
        {    0,  931,   76,   71 },  // subType 0x2c
};

// ------------------------------------------------------------
// per-type accessor arrays
// ------------------------------------------------------------
// FUN_100033440 (Item::getName) reads pool[type][subType][cosmeticNameIdx]
// from DAT_10007e220; FUN_10003326c (warnLine1 lookup) reads
// rects[type][subType] from DAT_100079ed0/250/5d0. these arrays give a
// single 3-entry indirection per lookup matching that vector layout.

inline constexpr const CosmeticNameRow* COSMETIC_NAMES[3] = {
    COSMETIC_NAMES_TYPE0,
    COSMETIC_NAMES_TYPE1,
    COSMETIC_NAMES_TYPE2,
};

inline constexpr int COSMETIC_NAMES_COUNT[3] = { 56, 56, 45 };

inline constexpr const IconRect* ICON_RECTS[3] = {
    ICON_RECT_TYPE0,
    ICON_RECT_TYPE1,
    ICON_RECT_TYPE2,
};

inline constexpr int ICON_RECTS_COUNT[3] = { 56, 56, 45 };
