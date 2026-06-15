#pragma once

// reconstructed from Ghidra:
//   FUN_1000412bc, DialogPanel::showHint dispatcher. indexes the pointer
//                   table at PTR_s_This_is_you__10007d108 (VA 0x10007d108) as a
//                   flat array of 8-byte pointers: for hint id N,
//                     main    = base[N*3 + 0]
//                     button  = base[N*3 + 1]
//                     subtext = base[N*3 + 2]
//
// our port replaces the pointer table with the static array below (the strings
// are constant .rodata, so no runtime init is needed). pattern matches
// perk_table.h / snag_table.h: inline constexpr so the storage lives in one TU.
//
// 24 entries (hint ids 0x00..0x17), one per ambient-hint target in the
// tutorial cascade (FUN_10001980c / GameBoard::tickAmbientPickupHinting).
// subtext is the empty string for hints that only use two lines; no entry is
// null. the {A}/{D}/{H}/{C}/{X} tokens are literal in the binary strings;
// TextItem::setString (FUN_10002fae8) substitutes the stat icons downstream,
// so they're copied verbatim here.

struct TutorialHintEntry {
    const char* main;
    const char* button;
    const char* subtext;
};

// each entry's trigger is the live board condition checked by the cascade
// (FUN_10001980c). content-tile types: 2={A}ttack, 3={D}efence, 6={H}ealth,
// 5={C}ontrol, 4={X}P. anchor = where the popup points.
inline constexpr TutorialHintEntry TUTORIAL_HINT_TABLE[24] = {
    // 0x00: fires immediately on the first tutorial frame (no extra gate).
    //        anchor: the player avatar token.
    { "This is you.", "Tap this hint to continue.", "" },
    // 0x01: the starting rack tiles (slots 0 and 2) have finished sliding +
    //        rotating into place. anchor: rack slot 2.
    { "These are tiles that you're holding.", "Drag a tile up to place it.", "" },
    // 0x02: the player has picked a rack tile to place (selectedRackSlot set)
    //        and it has settled into the placement preview. anchor: that tile.
    { "Drag the rotate button to rotate the placed tile.", "Tap the confirm button to lock it in and end your turn.", "" },
    // 0x03: the picked-up tile carries Attack {A} content (type 2). anchor: it.
    { "Place {A} tiles to gain Attack.", "Attack is used to damage snags.", "" },
    // 0x04: the picked-up tile carries Defence {D} content (type 3). anchor: it.
    { "Place {D} tiles to gain Defence.", "Defence reduces damage you receive.", "" },
    // 0x05: the picked-up tile carries Health {H} content (type 6). anchor: it.
    { "Place {H} tiles to restore your Health.", "If your Health is at its maximum, {H} tiles do nothing.", "" },
    // 0x06: the actively-dragged tile carries Control {C} content (type 5).
    //        anchor: that tile.
    { "Place {C} tiles on {C} spots to gain Control.", "{C} spots are shown when you hold a {C} tile.", "Accumulate 10 Control to upgrade your items." },
    // 0x07: an Experience {X} tile (type 4, e.g. dropped by a defeated snag)
    //        sits on the placed trail and isn't mid-animation. anchor: it.
    { "Defeated snags drop {X} on their tile.", "When Nemesis consumes {X} tiles, you gain Experience.", "Accumulate 10 Experience to level up your abilities." },
    // 0x08: the exit-direction compass arrow has appeared (player is far from
    //        the exit). anchor: the arrow.
    { "Form a path in the direction of the compass to exit this world.", "Each world gets more dangerous the longer you stay.", "" },
    // 0x09: a Snag tile is sitting (settled) in a rack slot. anchor: that tile.
    { "This is a Snag tile.", "Placing a Snag will cause it to attack you.", "" },
    // 0x0a: a Special Snag (snag type != 1) is settled in a rack slot.
    //        anchor: that tile.
    { "This is a Special Snag.", "Special Snags are tougher and have special", "abilities, but drop more {X} when defeated." },
    // 0x0b: the player has tapped a snag to inspect it; the DetailPanel
    //        combat-deadliness card is up and settled. anchor: the card.
    { "This shows how deadly the snag is with current {A} {H} {D}", "Red number shows how many turns you will defeat it in.", "Blue number shows how many turns it will defeat you in." },
    // 0x0c: the player has staged a tile for discard (discard list non-empty).
    //        anchor: the discard button.
    { "This button is used to discard tiles you don't want.", "Select the tiles you want to discard and tap this", "again to discard them." },
    // 0x0d: one of the discard-staged tiles holds a live snag (discarding a
    //        snag specifically). anchor: that snag tile.
    { "Discarding snags gives Nemesis", "experience, so do this sparingly.", "" },
    // 0x0e: Nemesis has become visible on the board. anchor: Nemesis.
    { "This is Nemesis.", "It chases you when you discard tiles.", "The game ends when Nemesis consumes you." },
    // 0x0f: the player has opened the stats menu (UserStatsPanel up + settled).
    //        anchor: the menu.
    { "This menu shows your current", "stats and the items you're using.", "" },
    // 0x10: the player has opened the pause/settings menu (PauseMenu up +
    //        settled). anchor: the menu.
    { "This is the settings menu. Here you can", "disable these tips or forfeit the current game.", "" },
    // 0x11: the level-up panel has opened (collected 10 {X}). anchor: the panel.
    { "You've collected 10 {X} and leveled up!", "Choose 1 stat to increase and one perk to gain.", "" },
    // 0x12: the item-choice panel has opened (collected 10 {C}). anchor: it.
    { "You've collected 10 {C} and found a new item!", "Choose which item you want to replace a current item.", "" },
    // 0x13: the event-choice panel has opened (EventChoicePanel up + settled).
    //        anchor: the panel.
    { "Events are one-shot abilities you can use", "once they're charged up. The symbol on each", "event shows how to charge it." },
    // 0x14: the player holds at least one event in the HUD tray and the tray
    //        icons have settled (none still sliding in). anchor: first slot.
    { "Your current events are held here.", "An event can be activated once its charges are filled.", "" },
    // 0x15: the player's health has hit 0 (playerDowned). anchor: the HUD.
    { "Your health is at 0.", "At this point, Nemesis will devour {X} tiles according to its level.", "If Nemesis doesn't consume you, your health will be refilled." },
    // 0x16: a dead end: the X-button (advance-Nemesis / pass-turn) is showing
    //        and the board is idle. anchor: the X-button.
    { "Your path has reached a dead end.", "Press this button to advance Nemesis to your tile.", "" },
    // 0x17: wrap-up: fires once the four stat-tile hints (0x03-0x06,
    //        {A}/{D}/{H}/{C}) have all been shown. anchor: the HUD.
    { "You can tap these and other icons to learn how they work.", "Experiment with the game to discover the rules & strategies!", "" },
};
