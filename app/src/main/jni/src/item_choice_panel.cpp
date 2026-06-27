#include "item_choice_panel.h"
#include "detail_panel.h"
#include "game.h"
#include "player_system.h"
#include "random.h"
#include "renderer.h"
#include "sound_queue.h"
#include <cstdint>
#include <vector>

// pixel-snap helper from menu.cpp (binary's FUN_100057374).
extern float snapMenuPixel(float v);

// ItemChoicePanel::init, FUN_100033fe0.
//
// per-slot init pattern (run for slots 0..3):
//   1. construct embedded widgets (Label::init on heldLabel /
//      candidateLabel / candidateBgLabel, TextItem::init on
//      candidateNameText + descText[0..1]). the heldDividerQuad's C++
//      member-init already ran its default ctor.
//   2. clear heldItem / candidateItem pointers (will be allocated in open).
//   3. add 9-slice glyph data to heldLabel (6 glyphs, atlas (70, 677)),
//      candidateLabel (3 glyphs, atlas (488, 81)), and candidateBgLabel
//      (3 glyphs, atlas (530, 81)).
//   4. size + position the three Labels using widthAccum/leftX-derived
//      coordinates; per-slot Y stride = (slotIdx * 126px) / 640.
//   5. set up heldDividerQuad UVs (atlas (231.875..277.5, 455.0..590.0))
//      and addVertexOffset (0, -0.028).
//   6. wire TextItem glyph tables to game's panel font (Game+0x10) and
//      set scales: candidateNameText = 0.085, descText = 0.07.
//
// binary constants (DAT_10005a180..194):
//   - DAT_180 = 640.0  (pixel-per-screen-unit divisor)
//   - DAT_184 = 0.046875  (heldLabel Y offset from per-slot Y)
//   - DAT_188 = 0.0484375 (heldLabel X position)
//   - DAT_18C = 0.575    (candidateBgLabel size value)
//   - DAT_190 = 0.0546875 (candidateLabel Y offset)
//   - DAT_194 = 0.25313  (candidateLabel X position)

void ItemChoicePanel::init(DetailPanel* heldItemRefIn) {

    // ---- (1) Menu chrome ----
    // title-strip atlas rect (641, 857, 269, 48): the "DREAM" header glyph
    // texture for the item-choice screen.
    Menu::init(0x281, 0x359, 0x10d, 0x30);

    // store back-pointer so open/update/close can hand off to the detail
    // panel for the held-item tooltip.
    heldItemRef = heldItemRefIn;

    // ---- (2) per-slot widget second-stage init ----
    // C++ member init has already run default ctors on Label / TextItem /
    // Quad; here we run the binary's second-stage init that sets default
    // sizes, colors, and (for TextItem) scaleX/Y = 1.0 + color = white.
    for (int i = 0; i < MAX_SLOT_COUNT; ++i) {
        ItemChoiceSlot& slot = slots[i];

        slot.heldLabel.init();              // FUN_10004c014
        // heldDividerQuad uses C++ default ctor (FUN_100007d78). no init.
        slot.candidateLabel.init();         // FUN_10004c014
        slot.candidateBgLabel.init();       // FUN_10004c014
        slot.candidateNameText.init();      // FUN_10002fa08
        slot.descText[0].init();            // FUN_10002fa08
        slot.descText[1].init();            // FUN_10002fa08
    }

    // ---- (3..6) per-slot glyph + layout pass ----
    Game* g = getGame();

    constexpr float SCREEN_REF        = 640.0f;       // DAT_10005a180
    constexpr float HELD_Y_BASE       = 0.046875f;    // DAT_10005a184
    constexpr float HELD_X            = 0.04843750f;  // DAT_10005a188
    constexpr float BG_SIZE_VAL       = 0.575f;       // DAT_10005a18c
    constexpr float CAND_Y_BASE       = 0.0546875f;   // DAT_10005a190
    constexpr float CAND_X            = 0.25312501f;  // DAT_10005a194
    constexpr float SLOT_Y_STRIDE_PX  = 126.0f;       // per-slot pixel stride

    // TextItem-internal scale floats: candidateNameText = 0.085,
    // descText[0..1] = 0.07. these go directly into TextItem+0x68 / +0x6C
    // (= scaleX, scaleY).
    constexpr float NAME_SCALE = 0.085f;
    constexpr float DESC_SCALE = 0.07f;

    for (int i = 0; i < MAX_SLOT_COUNT; ++i) {
        ItemChoiceSlot& slot = slots[i];

        // (2 continued) initial Item slots empty
        slot.heldItem      = nullptr;
        slot.candidateItem = nullptr;

        // ---- (3a) heldLabel: 6-glyph 9-slice frame, atlas (70..118, 677..725) ----
        // line 0: top edge (corner + edge)
        slot.heldLabel.pendingLineIndex = 0;
        {
            float origin[2] = { 70.0f, 677.0f };
            float size[2]   = { 24.0f, 24.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 2, off);
        }
        {
            float origin[2] = { 94.0f, 677.0f };
            float size[2]   = { 1.0f, 24.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 1, off);
        }
        slot.heldLabel.pendingLineIndex += 1;

        // line 1: middle (corner + center)
        {
            float origin[2] = { 70.0f, 701.0f };
            float size[2]   = { 24.0f, 1.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 0, off);
        }
        {
            float origin[2] = { 94.0f, 701.0f };
            float size[2]   = { 1.0f, 1.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 3, off);
        }
        slot.heldLabel.pendingLineIndex += 1;

        // line 2: bottom edge (corner + edge)
        {
            float origin[2] = { 70.0f, 702.0f };
            float size[2]   = { 24.0f, 24.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 2, off);
        }
        {
            float origin[2] = { 94.0f, 702.0f };
            float size[2]   = { 1.0f, 24.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.heldLabel.addGlyph(-1.0f, origin, size, 1, off);
        }

        // ---- (3b) candidateLabel: 3-glyph frame, atlas (488..529, 81..205) ----
        {
            float origin[2] = { 488.0f, 81.0f };
            float size[2]   = { 20.0f, 124.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateLabel.addGlyph(-1.0f, origin, size, 2, off);
        }
        {
            float origin[2] = { 508.0f, 81.0f };
            float size[2]   = { 1.0f, 124.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateLabel.addGlyph(-1.0f, origin, size, 1, off);
        }
        {
            float origin[2] = { 509.0f, 81.0f };
            float size[2]   = { 20.0f, 124.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateLabel.addGlyph(-1.0f, origin, size, 2, off);
        }

        // ---- (3c) candidateBgLabel: 3-glyph chrome, atlas (530..571, 81..204) ----
        {
            float origin[2] = { 530.0f, 81.0f };
            float size[2]   = { 20.0f, 123.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateBgLabel.addGlyph(-1.0f, origin, size, 2, off);
        }
        {
            float origin[2] = { 550.0f, 81.0f };
            float size[2]   = { 1.0f, 123.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateBgLabel.addGlyph(-1.0f, origin, size, 1, off);
        }
        {
            float origin[2] = { 551.0f, 81.0f };
            float size[2]   = { 20.0f, 123.0f };
            GlyphOffset off = { 0.0f, 0.0f };
            slot.candidateBgLabel.addGlyph(-1.0f, origin, size, 2, off);
        }

        // ---- (4) heldLabel: size + position ----
        // setSize(0.140625, 0.2109375): wide enough for the held-item card.
        // setPos((slotY + 0.046875) using HELD_X).
        slot.heldLabel.setSize(0.140625f, 0.2109375f);

        const float slotYNorm = (float)(i * (int)SLOT_Y_STRIDE_PX) / SCREEN_REF;
        const float heldY     = slotYNorm + HELD_Y_BASE;
        slot.heldLabel.setPosition(HELD_X, heldY);

        // ---- (5) heldDividerQuad setup ----
        // uv_min = (0.18164, 0.44434), uv_max = (0.22656, 0.57617). that's a
        // 0.04492 x 0.13184 UV box = 46 x 135 atlas pixels, matching the
        // displayed (0.071875, 0.2109375) at 640-px reference (= 46 x 135 px
        // 1:1). narrow vertical divider art positioned just right of the
        // heldLabel.
        slot.heldDividerQuad.setTexCoords(
            0.18164063f, 0.44433594f,    // uv_min
            0.22656250f, 0.57617188f);   // uv_max
        slot.heldDividerQuad.setSize(0.071875f, 0.2109375f);
        // posX = heldLabel.leftX + heldLabel.getWidth() + dividerWidth*0.5
        //        (= just right of the held label, centered on divider width).
        // posY = heldLabel.topY + heldLabel.getHeight() * 0.5
        //        (= vertical center of heldLabel; matches binary's centerY
        //         from the dropped-s1 return of getLeftX).
        const float heldLeftX  = slot.heldLabel.getLeftX();
        const float heldWidth  = slot.heldLabel.getWidth();
        const float heldHeight = slot.heldLabel.getHeight();
        slot.heldDividerQuad.posX = heldLeftX + heldWidth +
                                    slot.heldDividerQuad.width * 0.5f;
        slot.heldDividerQuad.posY = slot.heldLabel.topY + heldHeight * 0.5f;

        // ---- (4 cont.) candidateLabel: setSize + position ----
        // measureGlyphRun(-1, -1) gets the natural width + height of the
        // 3-glyph 9-slice frame; the heightTotal (s1) component feeds setSize
        // as the height (Ghidra drops s1; the asm captures it across s0 being
        // overwritten by DAT_18C = 0.575). result: setSize(0.575, heightTotal).
        // load-bearing: without the measured height the label renders with a
        // zero-or-tiny vertical extent and the candidate-side hit-test fails.
        const GlyphRunMetrics candNaturalRun = slot.candidateLabel.measureGlyphRun(-1, -1);
        slot.candidateLabel.setSize(BG_SIZE_VAL, candNaturalRun.heightTotal);

        const float candY = slotYNorm + CAND_Y_BASE;
        slot.candidateLabel.setPosition(CAND_X, candY);

        // ---- (4 cont.) candidateBgLabel: size + position mirror candidateLabel ----
        // binary's getWidth call returns paired (width, height); both are
        // forwarded as setSize args, with the leftX from a second
        // getLeftX call used as the new label's X position. Y matches
        // the candidate label.
        const float candWidth  = slot.candidateLabel.getWidth();
        const float candHeight = slot.candidateLabel.getHeight();
        slot.candidateBgLabel.setSize(candWidth, candHeight);

        const float candLeft   = slot.candidateLabel.getLeftX();
        slot.candidateBgLabel.setPosition(candLeft, candY);

        // ---- (6) TextItem glyph tables + scales ----
        // candidateNameText uses the panel font (game+0x10), large scale 0.085.
        slot.candidateNameText.glyphTablePtr = g->bmfontTablePtr(0);
        slot.candidateNameText.scaleX = NAME_SCALE;
        slot.candidateNameText.scaleY = NAME_SCALE;

        // descText[0..1] also use panel font, smaller scale 0.07 each.
        for (int d = 0; d < 2; ++d) {
            slot.descText[d].glyphTablePtr = g->bmfontTablePtr(0);
            slot.descText[d].scaleX = DESC_SCALE;
            slot.descText[d].scaleY = DESC_SCALE;
        }
    }

    // panel-level commit state
    slotCount        = 0;
    lastSelectedSlot = -1;
    selectedItem     = nullptr;
}

// ItemChoicePanel::pickItemSet, FUN_100035218.
//
// builds 3..4 candidate Items based on the player's current baseItems.
// each candidate's stat block is computed as:
//   candidate.stat = held.stat + flag
// where each per-stat flag is one of:
//   - primary stat (= ATK for type 0, HP for type 1, DEF for type 2):
//       flag = abilityCount (+ optional +1 / +2 bumps when a secondary
//       stat's downgrade roll wins, see below)
//   - secondary stat (when held.secondary == 0):
//       flag = 0 (no roll fires; starter items hit this path)
//   - secondary stat (when held.secondary > 0):
//       flag = 0xFFFFFFFFu (= -1 as int)  with 25% probability  (= "downgrade by 1")
//       flag = 0                          with 75% probability  (= no change)
//   - secondary stat (extra slot bonus path):
//       flag = 1 (= +1 unconditional bonus, replaces the downgrade roll)
//
// when perk 0x17 is owned, a 4th "extra" candidate slot is added with a
// random type. the extra slot's stat block has a 50/50 chance of giving
// a +1 bonus to one of its two secondary stats.
//
// abilityCount itself comes from perk 0x15:
//   - lvl 3: always 2
//   - lvl 2: 75% chance of 2, else 1
//   - lvl 1: 25% chance of 2, else 1
//   - lvl 0: always 1
//
// the filterMask passed to Item::init is the held.subType, so the new
// candidate rolls a different subType than the player's currently-held
// item of the same type.
//
// the downgrade-roll +1 bump to the primary is the binary's intentional
// trade-off: on a previously-upgraded item (held secondary > 0), the
// new candidate may lose that secondary in exchange for an extra boost
// to its primary. on starter items (secondary = 0), the downgrade rolls
// never fire so the candidate is a pure upgrade of the primary.
void ItemChoicePanel::pickItemSet(PlayerSystem* playerSystem,
                                  std::vector<Item*>& outCandidates) {

    // a flag with all bits set = -1 as signed int. added to held.stat,
    // this produces (held.stat - 1) = the "downgrade by 1" outcome.
    constexpr uint32_t DOWNGRADE_FLAG = 0xFFFFFFFFu;

    outCandidates.clear();

    // ---- extra-slot type selection (perk 0x17) ----
    // perk 0x17 owned -> roll which type [0..2] gets a 4th slot. binary
    // uses sentinel 3 for "no extra slot"; we use -1 since extraSlotType
    // is only checked via equality with a value in [0..2].
    int extraSlotType;

    if (playerSystem->perkLevel(0x17) > 0) {
        extraSlotType = rngInt(0, 2, 2);

    } else {
        extraSlotType = -1;
    }

    // ---- build slot-type list ----
    // base order = [0, 1, 2]; when t == extraSlotType, push t a second
    // time (e.g. extraSlotType=1 -> [0, 1, 1, 2]). extraSlotPos records
    // the index of the duplicate so the per-slot loop knows which
    // iteration is the bonus-path slot.
    std::vector<int> slotTypes;
    int extraSlotPos = -1;

    for (int t = 0; t < 3; ++t) {
        slotTypes.push_back(t);

        if (t == extraSlotType) {
            // count after the first push = index the duplicate will occupy
            // (FUN_100035218: lsr x25,x9,#0x2 reads size including first copy).
            extraSlotPos = (int)slotTypes.size();
            slotTypes.push_back(t);
        }
    }

    // ---- per-slot stat-roll + Item allocation ----
    for (size_t slotIdx = 0; slotIdx < slotTypes.size(); ++slotIdx) {
        const int type = slotTypes[slotIdx];
        Item* held = playerSystem->baseItems[type];
        const bool isExtraSlot = (extraSlotPos == (int)slotIdx);

        // ---- abilityCount (perk 0x15) ----
        const int abilityPerkLvl = playerSystem->perkLevel(0x15);
        uint32_t abilityCount;

        if (abilityPerkLvl == 3) {
            abilityCount = 2;

        } else if (abilityPerkLvl == 2) {
            int roll = rngInt(0, 100, 2);
            abilityCount = (roll < 0x4B) ? 2u : 1u;

        } else if (abilityPerkLvl == 1) {
            int roll = rngInt(0, 100, 2);
            abilityCount = (roll < 0x19) ? 2u : 1u;

        } else {
            abilityCount = 1;
        }

        // ---- per-type stat flags ----
        uint32_t atkFlag = 0;
        uint32_t defFlag = 0;
        uint32_t hpFlag  = 0;

        if (type == 2) {
            // DEF item: DEF is primary
            defFlag = abilityCount;

            // extra-slot bonus: 50% chance of +1 HP, else +1 ATK
            // (and skip the ATK downgrade roll since ATK is already bumped).
            bool doAtkDowngradeRoll;

            if (isExtraSlot) {
                int roll = rngInt(0, 100, 2);

                if (roll < 0x33) {
                    hpFlag = 1;
                    doAtkDowngradeRoll = true;

                } else {
                    atkFlag = 1;
                    doAtkDowngradeRoll = false;
                }

            } else {
                doAtkDowngradeRoll = true;
            }

            // ATK downgrade roll (25% chance; only when held.atk > 0).
            // a win downgrades candidate.atk by 1 AND bumps def primary +1.
            if (doAtkDowngradeRoll) {

                if (held->atk == 0) {
                    atkFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        atkFlag = DOWNGRADE_FLAG;
                        defFlag = abilityCount + 1;

                    } else {
                        atkFlag = 0;
                    }
                }
            }

            // HP downgrade roll (skip if extra-slot already set +1 HP).
            if (hpFlag == 0) {

                if (held->hp == 0) {
                    hpFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        hpFlag   = DOWNGRADE_FLAG;
                        defFlag += 1;

                    } else {
                        hpFlag = 0;
                    }
                }
            }

        } else if (type == 1) {
            // HP item: HP is primary
            hpFlag = abilityCount;

            // extra-slot bonus: 50% chance of +1 ATK, else +1 DEF
            // (and skip the DEF downgrade roll).
            bool doDefDowngradeRoll;

            if (isExtraSlot) {
                int roll = rngInt(0, 100, 2);

                if (roll < 0x33) {
                    atkFlag = 1;
                    doDefDowngradeRoll = true;

                } else {
                    defFlag = 1;
                    doDefDowngradeRoll = false;
                }

            } else {
                doDefDowngradeRoll = true;
            }

            // DEF downgrade roll
            if (doDefDowngradeRoll) {

                if (held->def == 0) {
                    defFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        defFlag = DOWNGRADE_FLAG;
                        hpFlag  = abilityCount + 1;

                    } else {
                        defFlag = 0;
                    }
                }
            }

            // ATK downgrade roll (skip if extra-slot already set +1 ATK).
            if (atkFlag == 0) {

                if (held->atk == 0) {
                    atkFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        atkFlag = DOWNGRADE_FLAG;
                        hpFlag += 1;

                    } else {
                        atkFlag = 0;
                    }
                }
            }

        } else if (type == 0) {
            // ATK item: ATK is primary
            atkFlag = abilityCount;

            // extra-slot bonus: 50% chance of +1 HP, else +1 DEF
            // (and skip the DEF downgrade roll since DEF is already bumped).
            bool doDefDowngradeRoll;

            if (isExtraSlot) {
                int roll = rngInt(0, 100, 2);

                if (roll < 0x33) {
                    hpFlag = 1;
                    doDefDowngradeRoll = true;

                } else {
                    defFlag = 1;
                    doDefDowngradeRoll = false;
                }

            } else {
                doDefDowngradeRoll = true;
            }

            // DEF downgrade roll
            if (doDefDowngradeRoll) {

                if (held->def == 0) {
                    defFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        atkFlag += 1;
                        defFlag  = DOWNGRADE_FLAG;

                    } else {
                        defFlag = 0;
                    }
                }
            }

            // HP downgrade roll (skip if extra-slot already set +1 HP).
            if (hpFlag == 0) {

                if (held->hp == 0) {
                    hpFlag = 0;

                } else {
                    int roll = rngInt(0, 100, 2);

                    if (roll < 0x19) {
                        atkFlag += 1;
                        hpFlag   = DOWNGRADE_FLAG;

                    } else {
                        hpFlag = 0;
                    }
                }
            }
        }
        // (slotTypes only contains values 0..2 by construction, so no
        // fallthrough branch is needed.)

        // ---- allocate the candidate Item ----
        // stat additions use uint32_t to preserve wraparound when a flag
        // is DOWNGRADE_FLAG (= -1 modifier). filterMask = held.subType so
        // the new Item rolls a different subType than the held one.
        Item* candidate = new Item();
        candidate->init(
            playerSystem,
            (int)held->type,
            (int)((uint32_t)held->atk + atkFlag),
            (int)((uint32_t)held->def + defFlag),
            (int)((uint32_t)held->hp  + hpFlag),
            (uint32_t)held->subType);

        outCandidates.push_back(candidate);
    }
}

// ItemChoicePanel::open, FUN_100034d60.
//
// binary flow:
//   1. panelShow (sound 8) + sound 10 (menuItemsAppear).
//   2. heldItemRef->reset(0): dismiss any leftover detail-panel tooltip.
//   3. clear commit state (lastSelectedSlot = -1, selectedItem = nullptr).
//   4. pickItemSet -> 3..4 candidate Item*s.
//   5. per slot:
//      a. delete the prior open()'s heldItem + candidateItem (deferred-
//         cleanup; close() leaves them alive until next open's prologue).
//      b. allocate new heldItem as a copy of playerSystem.baseItems[type].
//      c. assign candidateItem = candidates[i] (ownership transfer).
//      d. position the held-item icon at the heldLabel's center plus a
//         small +X / -Y nudge; pixel-snap both axes.
//      e. position the candidate-item icon at the candidateLabel's leftX
//         plus a +X offset, and at the candidateLabel's center-Y plus the
//         same -Y nudge as the held side. pixel-snap both axes.
//      f. set candidateNameText scale = 0.085, setText to candidate.getName(),
//         position relative to candidateLabel. auto-shrink the text scale
//         when rendered width exceeds the panel's content budget.
//      g. set the two descText lines (= candidate.getDescriptionLine(0..1))
//         with per-line Y stride and a shared X offset.
//      h. setSlotSelected(i, false): clear any leftover selection tint.
//   6. Menu::setSize(0.884375, (slotCount * 126 + 69) / 640): panel grows
//      taller with more slots.
//   7. Menu::setAnchorY(slotCount == 3 ? 0.359375 : 0.293750): vertical
//      centering of the panel on screen.
//   8. vtable[3] initial update (= this->update(0.0, 0.0)) so the first
//      frame's tween state is fresh.
//
// the binary's FUN_10000aefc returns (centerX, centerY) as a paired-float
// (Ghidra drops centerY in its decompile); inlined here as
// leftX + width/2, topY + height/2.

void ItemChoicePanel::open(PlayerSystem* playerSystem) {

    panelShow();   // Menu::panelShow

    Game* g = getGame();

    if (g) {
        g->soundQueue.trigger(0x0A);   // FUN_100035ccc(g+0x3880, 10) = menuItemsAppear
    }

    // dismiss any prior detail-panel tooltip
    heldItemRef->reset(0);

    // commit state reset
    lastSelectedSlot = -1;
    selectedItem     = nullptr;

    // ---- build candidate list ----
    std::vector<Item*> candidates;
    pickItemSet(playerSystem, candidates);

    slotCount = (int32_t)candidates.size();

    if (slotCount > MAX_SLOT_COUNT) {
        slotCount = MAX_SLOT_COUNT;
    }

    // position-nudge DAT constants (DAT_10005a198..1AC).
    constexpr float HELD_X_NUDGE       = 0.023438f;     // DAT_198
    constexpr float Y_NUDGE            = -0.003125f;    // DAT_19C (held + candidate)
    constexpr float CAND_X_NUDGE       = 0.085938f;     // DAT_1A0
    constexpr float NAME_X_OFFSET      = 0.167188f;     // DAT_1A4 (candidateNameText.posX)
    constexpr float NAME_Y_OFFSET      = 0.0625f;       // DAT_1A8 (candidateNameText.posY)
    constexpr float NAME_WIDTH_BUDGET  = -0.03125f;     // DAT_1AC (budget margin)
    constexpr float DESC_Y_CONST_PX    = 72.0f;         // DAT_1B0
    constexpr float SCREEN_REF_PX      = 640.0f;        // DAT_1B4
    constexpr float DESC_X_OFFSET      = 0.164063f;     // descText.posX offset
    constexpr float DESC_Y_STRIDE_PX   = 25.0f;         // per-line Y stride (descIdx * 25 px)
    constexpr float PANEL_WIDTH        = 0.884375f;     // DAT_1B8
    constexpr float ANCHOR_Y_NON3      = 0.293750f;     // DAT_1C0 (slotCount != 3)
    constexpr float ANCHOR_Y_3SLOT     = 0.359375f;     // DAT_1C4 (slotCount == 3)
    constexpr float PANEL_H_PER_SLOT   = 126.0f;        // 0x7e
    constexpr float PANEL_H_CONST_PX   = 69.0f;         // 0x45

    // ---- per-slot setup ----
    for (int slotIdx = 0; slotIdx < slotCount; ++slotIdx) {
        ItemChoiceSlot& slot = slots[slotIdx];

        // (5a) free prior items, deferred from the last close()
        if (slot.heldItem != nullptr) {
            delete slot.heldItem;
            slot.heldItem = nullptr;
        }

        if (slot.candidateItem != nullptr) {
            delete slot.candidateItem;
            slot.candidateItem = nullptr;
        }

        // (5b) (5c) allocate new heldItem from playerSystem and adopt candidate
        Item* candidate  = candidates[slotIdx];
        Item* heldSource = playerSystem->baseItems[candidate->type];
        slot.heldItem      = new Item(*heldSource);
        slot.candidateItem = candidate;

        // (5d) held icon position = heldLabel center + small nudge, snapped.
        const float heldLeftX   = slot.heldLabel.getLeftX();
        const float heldWidth   = slot.heldLabel.getWidth();
        const float heldHeight  = slot.heldLabel.getHeight();
        const float heldCenterX = heldLeftX + heldWidth * 0.5f;
        const float heldCenterY = slot.heldLabel.topY + heldHeight * 0.5f;
        slot.heldPosX = snapMenuPixel(heldCenterX + HELD_X_NUDGE);
        slot.heldPosY = snapMenuPixel(heldCenterY + Y_NUDGE);

        // (5e) candidate icon position = candidateLabel.leftX + nudge,
        //      candidateLabel.centerY + same -Y nudge, both snapped.
        const float candLeftX   = slot.candidateLabel.getLeftX();
        const float candWidth   = slot.candidateLabel.getWidth();
        const float candHeight  = slot.candidateLabel.getHeight();
        const float candCenterY = slot.candidateLabel.topY + candHeight * 0.5f;
        slot.candidatePosX = snapMenuPixel(candLeftX + CAND_X_NUDGE);
        slot.candidatePosY = snapMenuPixel(candCenterY + Y_NUDGE);

        // (5f) candidateNameText: scale 0.085, set name, position relative to
        //      candidateLabel, auto-shrink scale if rendered width > budget.
        slot.candidateNameText.scaleX = 0.085f;
        slot.candidateNameText.scaleY = 0.085f;
        slot.candidateNameText.setString(candidate->getName(), -1);

        slot.candidateNameText.posX = candLeftX + NAME_X_OFFSET;
        slot.candidateNameText.posY = slot.candidateLabel.topY + NAME_Y_OFFSET;

        // budget = (candLeftX + candWidth) - nameTextPosX + NAME_WIDTH_BUDGET
        const float nameWidthBudget = (candLeftX + candWidth)
                                    - slot.candidateNameText.posX
                                    + NAME_WIDTH_BUDGET;
        // rendered width = renderedWidth (+0x78 of TextItem) * scaleX
        const float renderedW = slot.candidateNameText.renderedWidth
                              * slot.candidateNameText.scaleX;

        if (renderedW > nameWidthBudget) {
            // shrink both axes proportionally so the name fits the budget
            slot.candidateNameText.scaleX =
                (nameWidthBudget * slot.candidateNameText.scaleX) / renderedW;
            slot.candidateNameText.scaleY =
                (nameWidthBudget * slot.candidateNameText.scaleY) / renderedW;
        }

        // (5g) descText[0..1]: 25 px Y stride, shared X offset 0.164.
        for (int d = 0; d < 2; ++d) {
            slot.descText[d].setString(candidate->getDescriptionLine(d), -1);

            const float lineYPx = (float)(d * (int)DESC_Y_STRIDE_PX) + DESC_Y_CONST_PX;
            const float lineY   = lineYPx / SCREEN_REF_PX;
            slot.descText[d].posX = candLeftX + DESC_X_OFFSET;
            slot.descText[d].posY = slot.candidateLabel.topY + lineY;
        }

        // (5h) clear any leftover selection tint
        setSlotSelected(slotIdx, false);
    }

    // (6-7) Menu::setSizeAndCenterY: panel grows with slot count. binary
    // hardcodes ANCHOR_Y_3SLOT / ANCHOR_Y_NON3 for iOS (virtualHeight ~=
    // 1.5); the helper derives the anchor from runtime virtualHeight so
    // the panel stays vertically centered on any aspect ratio.
    const float panelH = ((float)slotCount * PANEL_H_PER_SLOT + PANEL_H_CONST_PX)
                       / SCREEN_REF_PX;
    Menu::setSizeAndCenterY(PANEL_WIDTH, panelH);
    (void)ANCHOR_Y_3SLOT;
    (void)ANCHOR_Y_NON3;

    // (8) vtable[3] initial update: kicks the per-frame tween so the first
    //     rendered frame is at the right animation phase.
    update(0.0f, 0.0f);
}

// ItemChoicePanel::update, FUN_100034b30.
//
// per-frame chrome update + tap dispatch. binary's flow:
//   1. Menu::baseUpdate(dt): fades animation, ticks confirm-button state.
//   2. early-out if not visible or fade-in (animTimer0) hasn't completed.
//   3. read touch state from Game (game+4 = inputState, +8/+C = touch X/Y).
//   4. on tap (inputState == 1): for each slot 0..slotCount-1, hit-test
//      the candidateLabel first; on hit, unselect any prior pick, play
//      sound 7 (re-tap same slot) or sound 5 (new slot), latch selection,
//      set the Menu base's readyByte (= confirm button enabled). then
//      hit-test the heldLabel and heldDividerQuad; on either hit, pop up
//      DetailPanel showing the held item card.
//   5. on release (inputState == 0): tear down the detail panel via
//      heldItemRef->reset(0).
void ItemChoicePanel::update(float dt, float touchInput) {
    (void)touchInput;
    baseUpdate(dt);

    if (!visible || animTimer0 < 1.0f) {
        return;
    }

    Game* g = getGame();

    if (!g) {
        return;
    }

    const int touchState = g->inputState();

    if (touchState == 1) {
        // tap: translate touch into panel-local coords (panel chrome
        // is drawn at (anchorX, anchorY) via Menu::draw).
        const float touchLocalX = g->touchX() - anchorX;
        const float touchLocalY = g->touchY() - anchorY;

        for (int i = 0; i < slotCount; ++i) {
            ItemChoiceSlot& slot = slots[i];

            // ---- candidate-side tap: select this candidate ----
            if (slot.candidateLabel.contains(touchLocalX, touchLocalY)) {

                if (lastSelectedSlot >= 0 && lastSelectedSlot != i) {
                    setSlotSelected(lastSelectedSlot, false);
                }

                int sound = (lastSelectedSlot == i) ? 7 : 5;
                g->soundQueue.trigger(sound);

                lastSelectedSlot = i;
                setSlotSelected(i, true);
                readyByte = 1;
                return;
            }

            // ---- held-side tap: pop up detail panel for held item ----
            if (slot.heldLabel.contains(touchLocalX, touchLocalY)
             || slot.heldDividerQuad.contains(touchLocalX, touchLocalY)) {

                // FUN_10003f2c0 call site (mode-2 Item card). args derived
                // from the disassembly (decompile drops getWidth's paired
                // height return into the stack):
                //   anchor[0] = heldLabel.centerX + panel.anchorX
                //   anchor[1] = heldLabel.centerY + panel.anchorY
                //   headerY   = heldLabel.height * 0.5
                // anchor is the heldLabel's screen-space center, not the
                // touch point; the tooltip docks to the slot regardless of
                // where inside the label the player tapped.
                const float heldLeftX   = slot.heldLabel.getLeftX();
                const float heldWidth   = slot.heldLabel.getWidth();
                const float heldHeight  = slot.heldLabel.getHeight();
                const float heldCenterX = heldLeftX + heldWidth * 0.5f;
                const float heldCenterY = slot.heldLabel.topY + heldHeight * 0.5f;

                float anchor[2];
                anchor[0] = heldCenterX + anchorX;
                anchor[1] = heldCenterY + anchorY;
                const float headerY = heldHeight * 0.5f;

                heldItemRef->populateForItem(headerY, anchor, slot.heldItem);
                return;
            }
        }

    } else if (touchState == 0) {
        // release: dismiss any popped-up detail panel.
        heldItemRef->reset(0);
    }
}

// ItemChoicePanel::draw, FUN_1000349ac.
//
// binary flow:
//   1. early-out on !visible.
//   2. Menu::draw (chrome).
//   3. glPushMatrix + glTranslatef(anchorX, anchorY): slot content is
//      drawn in panel-local space.
//   4. bindTexture(9): UI atlas, used by the slot's labels.
//   5. first per-slot pass: heldLabel.draw(), heldDividerQuad.draw(),
//      then either candidateBgLabel (if this slot is the selected one)
//      or candidateLabel (= the "unselected" chrome; selection swaps
//      the label without retinting).
//   6. second per-slot pass: glPushMatrix + translate(heldPosX, heldPosY)
//      and call the inline `drawItemIcon` helper for the held item with
//      ability-icon overlay at offset (0.0015625, 0.011); glPopMatrix.
//      same for candidate but without ability icons (offset (0, 0)).
//      then candidateNameText.draw() + descText[0..1].draw().
//   7. glPopMatrix.

namespace {

// inline port of FUN_1000332d4. draws an Item's visual stack:
//   - icon1 (silhouette) under texture 11 (items atlas).
//   - icon2 + tint1 (ATK number) under texture 9, gated on item.atk != 0.
//   - icon4 + tint3 (HP  number),               gated on item.hp  != 0.
//   - icon3 + tint2 (DEF number),               gated on item.def != 0.
//   - if `drawAbilities`: push matrix, translate by *abilityOffset,
//     draw each abilities[i].iconQuad whose abilityType != 0, pop.
//
// caller is expected to have already glTranslated to the item's screen
// origin. binary's atlas-binding is sticky (relies on the caller's
// bindTexture(9) to be re-asserted around tint draws).
void drawItemIcon(Item* item, bool drawAbilities, const float abilityOffset[2]) {
    bindTexture(11);
    item->icon1.quad.draw();
    bindTexture(9);

    if (item->atk != 0) {
        item->icon2.quad.draw();
        item->tint1.draw();
    }

    if (item->hp != 0) {
        item->icon4.quad.draw();
        item->tint3.draw();
    }

    if (item->def != 0) {
        item->icon3.quad.draw();
        item->tint2.draw();
    }

    if (drawAbilities) {
        glPushMatrix();
        glTranslatef(abilityOffset[0], abilityOffset[1], 0.0f);

        for (int i = 0; i < 2; ++i) {

            if (item->abilities[i].abilityType != 0) {
                item->abilities[i].iconQuad.draw();
            }
        }

        glPopMatrix();
    }
}

}  // anonymous namespace

void ItemChoicePanel::draw() {

    if (!visible) {
        return;
    }

    Menu::draw();

    glPushMatrix();
    glTranslatef(anchorX, anchorY, 0.0f);
    bindTexture(9);

    if (slotCount <= 0) {
        glPopMatrix();
        return;
    }

    // first pass: per-slot label chrome (held side always, candidate side
    // selected vs unselected).
    for (int i = 0; i < slotCount; ++i) {
        ItemChoiceSlot& slot = slots[i];
        slot.heldLabel.draw();
        slot.heldDividerQuad.draw();

        if (i == lastSelectedSlot) {
            slot.candidateBgLabel.draw();

        } else {
            slot.candidateLabel.draw();
        }
    }

    // second pass: per-slot Item icons + text.
    // held side draws ability icons offset by (1/640 right, 7/640 down).
    // candidate side draws without ability icons (flag = false, offset unused).
    constexpr float HELD_ABILITY_OFFSET[2]      = { 1.0f / 640.0f, 7.0f / 640.0f };
    constexpr float CANDIDATE_ABILITY_OFFSET[2] = { 0.0f, 0.0f };

    for (int i = 0; i < slotCount; ++i) {
        ItemChoiceSlot& slot = slots[i];

        glPushMatrix();
        glTranslatef(slot.heldPosX, slot.heldPosY, 0.0f);
        drawItemIcon(slot.heldItem, true, HELD_ABILITY_OFFSET);
        glPopMatrix();

        glPushMatrix();
        glTranslatef(slot.candidatePosX, slot.candidatePosY, 0.0f);
        drawItemIcon(slot.candidateItem, false, CANDIDATE_ABILITY_OFFSET);
        glPopMatrix();

        slot.candidateNameText.draw();
        slot.descText[0].draw();
        slot.descText[1].draw();
    }

    glPopMatrix();
}

// ItemChoicePanel::close, FUN_10003568c.
//
// gameBoardUpdate calls this after consuming selectedItem in the commit
// branch. binary is just 3 lines: hide chrome, reset detail panel, clear
// the commit selection. it does not delete slots[].heldItem or
// slots[].candidateItem; those persist across close -> open and only get
// freed by the next open()'s prologue cleanup. matches the binary's
// deferred-cleanup pattern (the same one LevelUpPanel uses).
void ItemChoicePanel::close() {
    panelHide(false);               // FUN_100057b1c(panel, 0)
    heldItemRef->reset(0);          // FUN_1000404b0 = DetailPanel::reset(0)
    selectedItem = nullptr;
}

// ItemChoicePanel::setSlotSelected, FUN_100034cc8.
//
// tints slot N's text into selected/unselected state. binary packs two
// uint32 RGBA writes:
//
//   candidateNameText (+0x74): pure greyscale
//     selected   -> (200, 200, 200, 255)  light gray
//     unselected -> (255, 255, 255, 255)  white
//
//   descText[0..1]  (+0x74): faint magenta-tinted (G slightly less than R=B)
//     selected   -> (150, 140, 150, 255)
//     unselected -> (200, 190, 200, 255)
//
// after each write the binary calls applyColor (FUN_100030144) to propagate
// the new RGBA onto every glyph quad already laid out by setString.
void ItemChoicePanel::setSlotSelected(int slotIdx, bool selected) {
    ItemChoiceSlot& slot = slots[slotIdx];

    const uint8_t nameRGB = selected ? 200 : 255;
    slot.candidateNameText.colorR = nameRGB;
    slot.candidateNameText.colorG = nameRGB;
    slot.candidateNameText.colorB = nameRGB;
    slot.candidateNameText.alpha  = 0xFF;
    slot.candidateNameText.applyColor();

    const uint8_t descRB = selected ? 0x96 : 0xC8;   // 150 / 200
    const uint8_t descG  = selected ? 0x8C : 0xBE;   // 140 / 190

    for (int i = 0; i < 2; ++i) {
        TextItem& t = slot.descText[i];
        t.colorR = descRB;
        t.colorG = descG;
        t.colorB = descRB;
        t.alpha  = 0xFF;
        t.applyColor();
    }
}

// ItemChoicePanel::setAlpha, FUN_1000356c0, vtable[4].
//
// Menu::setAlpha first (chrome: bgDim half-alpha + frame9slice / title /
// closeBg / confirmButton), then per active slot propagate alpha onto:
//   heldItem (Item::setAlpha = FUN_1000333a8; every sub-Quad + tint)
//   heldLabel (Label::setAlpha = FUN_10004c84c)
//   heldDividerQuad (Quad::setAlpha = FUN_100008388)
//   candidateItem (Item::setAlpha)
//   candidateLabel (Label::setAlpha)
//   candidateBgLabel (Label::setAlpha)
//   candidateNameText (TextItem::setAlpha = FUN_1000301fc)
//   descText[0..1] (TextItem::setAlpha)
//
// the binary unconditionally dereferences heldItem / candidateItem here:
// open() always allocates both pointers per active slot before the panel
// becomes visible, so they're never null while setAlpha is reachable.
void ItemChoicePanel::setAlpha(uint8_t a) {
    Menu::setAlpha(a);

    for (int i = 0; i < slotCount; ++i) {
        ItemChoiceSlot& slot = slots[i];

        slot.heldItem->setAlpha(a);
        slot.heldLabel.setAlpha(a);
        slot.heldDividerQuad.setAlpha(a);
        slot.candidateItem->setAlpha(a);
        slot.candidateLabel.setAlpha(a);
        slot.candidateBgLabel.setAlpha(a);
        slot.candidateNameText.setAlpha(a);
        slot.descText[0].setAlpha(a);
        slot.descText[1].setAlpha(a);
    }
}

// ItemChoicePanel::onConfirmTapped, vtable[5].
//
// fires when the player taps confirm. latches selectedItem from the
// last-selected slot's candidate. consumed by gameBoardUpdate commit.
void ItemChoicePanel::onConfirmTapped() {

    if (lastSelectedSlot >= 0 && lastSelectedSlot < slotCount) {
        selectedItem = slots[lastSelectedSlot].candidateItem;
    }
}
