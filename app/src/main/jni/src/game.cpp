#include "game.h"
#include "title_menu.h"
#include "game_board.h"
#include "platform.h"
#include "audio_engine.h"
#include "renderer.h"
#include "quad.h"
#include <SDL.h>
#include <GLES/gl.h>
#include "random.h"
#include "player_system.h"
#include "snag_content.h"
#include "snag_table.h"
#include "tile_content.h"
#include "tile_object.h"
#include <ctime>
#include <cstdio>

// virtual screen height: computed at runtime as screenHeight / screenWidth.
// DAT_10007ddb8 in the binary is a global that gets set by FUN_100044764.
// we store it here and update it from Renderer::getVirtualHeight() after init.
static float sVirtualHeight = 1.5f;

// back-button-only transition target (no binary equivalent): character
// select -> title. extends the binary's transitionTarget cases 0..9 with 10.
static constexpr int kBackWorldToTitle = 10;

// set while the character-select screen is fading out for a back-button exit.
// Game::update drives World::tickFadeOut each frame; once the content is black
// it fires the kBackWorldToTitle curtain. one Game instance, so a file-static is
// safe glue state (no room in the binary-exact Game / World structs).
static bool sCharSelectExiting = false;

// global game-singleton pointer, mirroring the binary's DAT_10007e868.
// set by Game::create, accessed by subsystems via getGame() (= FUN_100043798).
static Game* sGameInstance = nullptr;

Game* getGame() {
    return sGameInstance;
}

// max delta time, confirmed from binary DAT_10005a4a8 = 0.1
static const float MAX_DELTA_TIME = 0.1f;

// bindTexture moved to renderer.cpp.

// texture loading order (confirmed from binary string table at 0x6552E)
// GL IDs are assigned sequentially starting from 1 in this order
static const char* sTextureFiles[GAME_NUM_TEXTURES] = {
    "font.png",       // GL ID 1
    "fontClean.png",  // GL ID 2
    "fontWorld.png",  // GL ID 3
    "tiles1.png",     // GL ID 4
    "tiles2.png",     // GL ID 5
    "tiles3.png",     // GL ID 6
    "tiles4.png",     // GL ID 7
    "sheet1.png",     // GL ID 8
    "ui1.png",        // GL ID 9
    "snags1.png",     // GL ID 10
    "items1.png",     // GL ID 11
    "icons1.png",     // GL ID 12
    "icons2.png"      // GL ID 13
};

// complete sound slot mapping (confirmed from binary string table at 0x5F5D4)
// base names only; variants are loaded as "{base}1.wav", "{base}2.wav", etc.
static const char* sSoundBaseNames[] = {
    "titleButtonPress",    //  0
    "titleButtonRelease",  //  1
    "titleButtonCancel",   //  2
    "wipeOn",              //  3
    "wipeOff",             //  4
    "buttonDown",          //  5
    "buttonUp",            //  6
    "buttonCancel",        //  7
    "menuAppear",          //  8
    "menuEventsAppear",    //  9
    "menuItemsAppear",     // 10
    "menuLevelAppear",     // 11
    "tutorial",            // 12
    "tutorialAccept",      // 13
    "tileGrab",            // 14
    "tilePlace",           // 15
    "tileSelect",          // 16
    "tileDraw",            // 17
    "disabled",            // 18
    "tooltipAppear",       // 19
    "discard",             // 20
    "attack",              // 21
    "defence",             // 22
    "health",              // 23
    "damage",              // 24
    "blank",               // 25
    "control",             // 26
    "experience",          // 27
    "snagAttackHit",       // 28
    "snagAttackMiss",      // 29
    "snagDead",            // 30
    "snagMove",            // 31
    "snagPlace",           // 32
    "snagSpecAbility",     // 33
    "snagSpecAppear",      // 34
    "snagSpecDead",        // 35
    "snagSpecPlace",       // 36
    "attackHit",           // 37
    "attackMiss",          // 38
    "nemesisAppear",       // 39
    "nemesisEat",          // 40
    "nemesisEatBad",       // 41
    "nemesisEatGood",      // 42
    "nemesisLevel",        // 43
    "nemesisXp",           // 44
    "eventActivate",       // 45
    "eventActivateSelect", // 46
    "eventCharge",         // 47
    "eventReady",          // 48
    "death",               // 49
    "scoreAppear",         // 50
    "keyAppear",           // 51
    "heartbeat",           // 52
    "refresh",             // 53
    "shimmer",             // 54
    "keyDrop",             // 55
    "unlockLight",         // 56
    "lockTravel",          // 57
    "lockUnlock",          // 58
    "unlockAppear",        // 59
    "freeze",              // 60
    "trapAppear",          // 61
    "trapPlace",           // 62
    "exitUnlocked",        // 63
    "exitPlayer",          // 64
    "exitTransition",      // 65
    "tileBadLuck",         // 66
    "tileBarricade",       // 67
    "tileEffortGrow",      // 68
    "tileEffortPlace",     // 69
    "tileForesight",       // 70
    "tileLure",            // 71
    "tileMemento",         // 72
    "tileMilestone",       // 73
    "tilePainGrow",        // 74
    "tilePainPlace",       // 75
    "tilePause",           // 76
    "tilePotential",       // 77
    "tileSecret",          // 78
    "tileWarmth",          // 79
    "tileWealth",          // 80
    "achievement",         // 81
};
#define NUM_SOUND_SLOTS 82

// audio handles for loaded sound effects
// each slot can have up to MAX_SOUND_VARIANTS loaded (random selection at playback)
#define MAX_SOUND_VARIANTS 4
static int sSoundHandles[SOUND_QUEUE_SLOTS][MAX_SOUND_VARIANTS];
static int sSoundVariantCount[SOUND_QUEUE_SLOTS];

// per-slot sound gains (from DAT_10007a8ac in binary, stride 0x10, all 82 values)
// gain is scaled by 1.3 (DAT_10005a1d8) before being applied to SDL_mixer
static const float SOUND_GAIN_SCALE = 1.3f;
static const float sSoundGains[NUM_SOUND_SLOTS] = {
    0.5f,   //  0 titleButtonPress
    0.7f,   //  1 titleButtonRelease
    0.1f,   //  2 titleButtonCancel
    0.05f,  //  3 wipeOn
    0.05f,  //  4 wipeOff
    0.2f,   //  5 buttonDown
    0.2f,   //  6 buttonUp
    0.5f,   //  7 buttonCancel
    1.0f,   //  8 menuAppear
    0.1f,   //  9 menuEventsAppear
    0.4f,   // 10 menuItemsAppear
    0.3f,   // 11 menuLevelAppear
    0.3f,   // 12 tutorial
    0.1f,   // 13 tutorialAccept
    0.1f,   // 14 tileGrab
    0.2f,   // 15 tilePlace
    0.05f,  // 16 tileSelect
    0.3f,   // 17 tileDraw
    0.2f,   // 18 disabled
    0.1f,   // 19 tooltipAppear
    0.7f,   // 20 discard
    0.3f,   // 21 attack
    0.1f,   // 22 defence
    0.3f,   // 23 health
    0.5f,   // 24 damage
    0.1f,   // 25 blank
    0.4f,   // 26 control
    0.15f,  // 27 experience
    0.15f,  // 28 snagAttackHit
    0.05f,  // 29 snagAttackMiss
    0.1f,   // 30 snagDead
    0.07f,  // 31 snagMove
    0.5f,   // 32 snagPlace
    0.6f,   // 33 snagSpecAbility
    0.15f,  // 34 snagSpecAppear
    0.2f,   // 35 snagSpecDead
    0.03f,  // 36 snagSpecPlace
    0.3f,   // 37 attackHit
    0.5f,   // 38 attackMiss
    0.2f,   // 39 nemesisAppear
    0.05f,  // 40 nemesisEat
    0.1f,   // 41 nemesisEatBad
    0.15f,  // 42 nemesisEatGood
    0.3f,   // 43 nemesisLevel
    0.15f,  // 44 nemesisXp
    0.6f,   // 45 eventActivate
    0.2f,   // 46 eventActivateSelect
    0.15f,  // 47 eventCharge
    0.2f,   // 48 eventReady
    1.0f,   // 49 death
    0.3f,   // 50 scoreAppear
    0.2f,   // 51 keyAppear
    1.5f,   // 52 heartbeat
    1.0f,   // 53 refresh
    0.1f,   // 54 shimmer
    0.7f,   // 55 keyDrop
    0.05f,  // 56 unlockLight
    0.2f,   // 57 lockTravel
    0.3f,   // 58 lockUnlock
    1.0f,   // 59 unlockAppear
    0.4f,   // 60 freeze
    0.3f,   // 61 trapAppear
    0.2f,   // 62 trapPlace
    0.1f,   // 63 exitUnlocked
    0.2f,   // 64 exitPlayer
    0.5f,   // 65 exitTransition
    0.4f,   // 66 tileBadLuck
    0.3f,   // 67 tileBarricade
    0.5f,   // 68 tileEffortGrow
    0.3f,   // 69 tileEffortPlace
    0.8f,   // 70 tileForesight
    0.1f,   // 71 tileLure
    0.2f,   // 72 tileMemento
    1.0f,   // 73 tileMilestone
    0.2f,   // 74 tilePainGrow
    0.1f,   // 75 tilePainPlace
    0.1f,   // 76 tilePause
    0.4f,   // 77 tilePotential
    0.4f,   // 78 tileSecret
    0.3f,   // 79 tileWarmth
    0.5f,   // 80 tileWealth
    0.3f    // 81 achievement
};

Game* Game::create() {
    Game* game = (Game*)SDL_malloc(sizeof(Game));

    if (!game) {
        SDL_Log("Failed to allocate Game struct (%d bytes)", (int)sizeof(Game));
        return nullptr;
    }

    // zero the whole struct (gameState_ is at offset 0). C++ container heads
    // (vectors, sets) inside this region get their RAII state restored via the
    // placement-new calls below.
    memset(game, 0, sizeof(Game));
    memset(game->textures, 0, sizeof(game->textures));

    // explicit non-zero defaults from the binary's Game ctor (FUN_1000437a4
    // tail). stashedDifficulty_ defaults to 1 = Normal; the binary's
    // ctor writes 1 here, making Normal the
    // out-of-box difficulty when no save state has run yet.
    game->stashedDifficulty() = 1;

    // mirror the binary's global game-singleton pattern (DAT_10007e868 stored
    // at module scope, retrieved via FUN_100043798). lets subsystems reach
    // the Game struct without each holding their own back-pointer.
    sGameInstance = game;

    // init utility subsystems
    game->soundQueue.init();
    game->musicController.init();
    game->overlay().init(sVirtualHeight);
    game->boardPtr() = nullptr;

    // post-run score panel: heap-allocated via operator_new(0x1018) (matches
    // binary's FUN_1000437a4 alloc + thunk_FUN_100010764 ctor).
    game->scorePanel() = new ScorePanel();
    game->scorePanel()->init();

    // per-difficulty stat-history: the binary's tracker ctor (FUN_100036c54,
    // invoked from FUN_1000437a4 with a `bl 0x100037114` thunk) loops over
    // the 3 list heads in the tracker and self-aliases each sentinel:
    //   *(puVar2 + 0) = puVar2;        // sentinel.prev = self
    //   *(puVar2 + 4) = puVar2;        // sentinel.next = self
    //   *(puVar2 + 8) = 0;             // count = 0
    // placement-new is the C++ equivalent: it runs ScoreHistory's default
    // ctor in-place, which fires each std::list's default ctor and gets the
    // same self-aliasing empty state. needed because our Game::create did
    // a memset(0) over the struct that wiped any earlier C++ init.
    new (&game->scoreHistory()) ScoreHistory();

    // Shop: same memset-vs-RAII issue. Shop owns a std::list (keyIcons),
    // 3 std::set (pools), and 3 std::vector (unlocks lists) whose
    // empty-sentinels were just wiped. placement-new restores the C++
    // defaults; Shop::init then ports the binary's FUN_1000502d4 chrome
    // construction (called from FUN_1000437a4 via thunk
    // 0x100051e1c).
    new (&game->shop()) Shop();
    game->shop().init();

    // LeaderboardMenu: header Label + close-X Quad + 3 per-difficulty
    // sections (each with 2 divider Quads + diffLabel TextItem + 5 ScoreRows
    // of Quad/Quad/TextItem[9]). all C++ subobjects' RAII state was wiped
    // by the surrounding memset; placement-new restores their vtables and
    // default-init their internal members. init() then runs the binary's
    // FUN_100036c54 visual setup tail (header chrome glyphs + close-X UV).
    new (&game->leaderboardMenu()) LeaderboardMenu();
    game->leaderboardMenu().init();

    // persistent shop save buffer (3 std::vector). same memset / RAII
    // fix applies; restore via placement-new of PersistentUnlocks.
    new (&game->shopSaveBuffer()) PersistentUnlocks();

    // leaderboard xfer buffer. std::vector<XferEntry>.
    new (&game->leaderboardSaveBuffer()) std::vector<LeaderboardMenu::XferEntry>();

    // save-slot scratch buffers (one std::vector<uint8_t> per slot; holds
    // the encoded blob between encode and disk write). all 5 lived under
    // the memset above; placement-new restores libc++'s empty-sentinel state.
    new (&game->saveScratch(0)) std::vector<uint8_t>();
    new (&game->saveScratch(1)) std::vector<uint8_t>();
    new (&game->saveScratch(2)) std::vector<uint8_t>();
    new (&game->saveScratch(3)) std::vector<uint8_t>();
    new (&game->saveScratch(4)) std::vector<uint8_t>();

    // GameSnapshot. owns 5 std::vector heads + 2 std::map
    // heads + a TileWeightPool, all RAII-managed C++ containers wiped
    // by the memset above, so placement-new restores their empty-
    // sentinel state.
    new (&game->gameSnapshot()) GameSnapshot();

    // AchievementSaveBuffer. owns 1 std::vector<int> + 4
    // std::set<int>. same RAII restore.
    new (&game->achievementSaveBuffer()) AchievementSaveBuffer();

    // per-slot format magic values. must be set before SaveSystem::load
    // runs (main.cpp) so the slot loaders' magic-match check sees the
    // correct in-memory value. bumping any of these in save_buffers.h
    // invalidates that slot's saved blobs on next launch.
    game->gameSnapshot().versionMagic           = SaveMagic::SAV;
    game->settingsMagic()                       = SaveMagic::SET;
    game->shopSaveBuffer().versionMagic         = SaveMagic::UNL;
    game->leaderboardSaveMagic()                = SaveMagic::SCO;
    game->achievementSaveBuffer().versionMagic  = SaveMagic::ACH;

    // first-launch defaults for the slot 1 (settings) fields. set before
    // SaveSystem::load so a stored save file can override them; the prior
    // Game::init writes to these have been removed because they would
    // clobber a freshly-loaded save.
    game->tutorialFlag()    = true;   // binary sets tutorialFlag = 1: tutorial on out of the box
    game->globalSeVolume()  = 0.8f;
    game->globalBgmVolume() = 0.5f;

    // AchievementTracker. owns a std::map<int,int> counters and
    // two std::list<int> notification queues whose RAII state the memset
    // above just wiped. placement-new restores their empty-sentinels.
    new (&game->achievementTracker()) AchievementTracker();

    // AchievementsMenu. 50 tiles, each with TextItem title /
    // description / progressText (std::string + std::vectors), shared
    // Label + Quads, and a std::map<int,int> sorted-display head, all
    // of which need their RAII members restored after the memset above.
    new (&game->achievementsMenu()) AchievementsMenu();
    game->achievementsMenu().init();

    // data[] is already zeroed by memset above. now construct subsystem objects.
    // (called from the game constructor FUN_1000437a4)
    game->titleMenu().construct();  // FUN_10004a774
    game->world().construct();      // FUN_100055268

    // mark all sound handles as unloaded
    for (int i = 0; i < SOUND_QUEUE_SLOTS; i++) {
        sSoundVariantCount[i] = 0;

        for (int v = 0; v < MAX_SOUND_VARIANTS; v++) {
            sSoundHandles[i][v] = -1;
        }
    }

    SDL_Log("Game struct allocated (%d bytes)", (int)sizeof(Game));
    return game;
}

void Game::destroy() {

    if (boardPtr()) {
        boardPtr()->destroy();
        boardPtr() = nullptr;
    }

    if (scorePanel()) {
        scorePanel()->clearKeysRow();
        delete scorePanel();
        scorePanel() = nullptr;
    }

    for (int i = 0; i < GAME_NUM_TEXTURES; i++) {

        if (textures[i]) {
            glDeleteTextures(1, &textures[i]);
        }
    }

    SDL_free(this);
}

bool Game::loadTextures() {

    for (int i = 0; i < GAME_NUM_TEXTURES; i++) {
        int w, h;
        textures[i] = Platform::loadTexture(sTextureFiles[i], &w, &h);

        if (!textures[i]) {
            SDL_Log("Failed to load texture %d: %s", i + 1, sTextureFiles[i]);
            return false;
        }

        SDL_Log("Texture %d (GL id %u): %s (%dx%d)", i + 1, textures[i], sTextureFiles[i], w, h);
    }

    return true;
}

bool Game::loadSounds() {
    // for each slot, try loading {baseName}1.wav, {baseName}2.wav, etc.
    // stop when a file fails to load (no more variants).
    // matches the original's load-time variant discovery.
    int totalLoaded = 0;

    for (int i = 0; i < NUM_SOUND_SLOTS; i++) {
        int count = 0;

        for (int v = 1; v <= MAX_SOUND_VARIANTS; v++) {
            char filename[80];
            snprintf(filename, sizeof(filename), "%s%d.wav", sSoundBaseNames[i], v);

            int handle = AudioEngine::loadSound(filename);

            if (handle < 0) {
                break;
            }

            sSoundHandles[i][count] = handle;
            count++;
        }

        sSoundVariantCount[i] = count;
        totalLoaded += count;
    }

    SDL_Log("Loaded %d sound files across %d slots", totalLoaded, NUM_SOUND_SLOTS);
    return true;
}

// FUN_100036990, parse a single .fnt file into a BMFontTable.
//
// the binary's parser uses fopen + fgets to read the AngelCode BMFont text
// format line-by-line. on Android, raw fopen on `assets/foo.fnt` won't
// work, so we go through Platform::loadAsset (which wraps SDL_RWops, the
// only mechanism that handles APK-bundled assets transparently). same
// per-line sscanf semantics, just a different read mechanism.
//
// table layout details + the per-char index formula `c*9 + 2..c*9 + 10`
// are documented in bmfont_table.h.
static bool parseFntFile(BMFontTable* table, const char* path, int textureIdx) {
    size_t fileSize = 0;
    unsigned char* fileData = Platform::loadAsset(path, &fileSize);

    if (!fileData) {
        SDL_Log("Failed to load font: %s", path);
        return false;
    }

    table->textureIndex = textureIdx;

    // per-table cursor-advance multiplier read by TextItem::setString. iOS
    // initializes this to 1.0 via FUN_100036958, a separate BMFontTable
    // initializer the Game ctor runs before parseFntFile. without the 1.0,
    // each char's cursor advance gets multiplied by 0 and glyphs collapse
    // onto each other (the cause of the "letters merged" rendering bug).
    table->perTableMul = 1.0f;

    int scaleW = 0, scaleH = 0, charsParsed = 0;

    const char* cursor = (const char*)fileData;
    const char* end = cursor + fileSize;

    while (cursor < end) {
        const char* lineEnd = cursor;

        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
            lineEnd++;
        }

        // line-by-line dispatch matching FUN_100036990's order. info /
        // page lines are skipped (binary's fgets discards them too).
        if (strncmp(cursor, "common ", 7) == 0) {
            sscanf(cursor, "common lineHeight=%d base=%d scaleW=%d scaleH=%d",
                   &table->lineHeight, &table->base, &scaleW, &scaleH);
        } else if (strncmp(cursor, "char ", 5) == 0) {
            int id = 0, x = 0, y = 0, w = 0, h = 0;
            int xOff = 0, yOff = 0, xAdv = 0;

            int matched = sscanf(cursor,
                "char id=%d x=%d y=%d width=%d height=%d "
                "xoffset=%d yoffset=%d xadvance=%d",
                &id, &x, &y, &w, &h, &xOff, &yOff, &xAdv);

            if (matched == 8 && id >= 0 && id < 128
                    && scaleW > 0 && scaleH > 0
                    && table->lineHeight > 0) {
                // fill char `id`'s glyph entry, pre-dividing by the atlas scale
                // and line height (mirrors the binary's per-char float writes).
                BMFontEntry& e = table->entries[id];
                float lineH = (float)table->lineHeight;
                e.uvU  = (float)x / (float)scaleW;
                e.uvV  = (float)y / (float)scaleH;
                e.uvU2 = e.uvU + (float)w / (float)scaleW;
                e.uvV2 = e.uvV + (float)h / (float)scaleH;
                e.sizeW = (float)w / lineH;
                e.sizeH = (float)h / lineH;
                e.xoffsetHalved  = (float)xOff / lineH * 0.5f;
                e.yoffsetCenter  = (float)yOff / lineH + e.sizeH * 0.5f
                                   - (float)table->base / lineH;
                e.xadvanceHalved = (float)xAdv / lineH * 0.5f;
                charsParsed++;
            }
        }

        cursor = lineEnd;
        while (cursor < end && (*cursor == '\n' || *cursor == '\r')) {
            cursor++;
        }
    }

    SDL_free(fileData);
    SDL_Log("Loaded font %d (%s): %d glyphs, lineHeight=%d, atlas=%dx%d",
            textureIdx, path, charsParsed, table->lineHeight, scaleW, scaleH);
    return true;
}

// FUN_1000461b4, orchestrator. iterate fonts 0..2, fetch the .fnt path
// from the binary's name table (PTR_s_font_1000748c0 -> "font" / "fontClean"
// / "fontWorld"), parse into the corresponding embedded BMFontTable.
bool Game::loadFonts() {
    static const char* kFontNames[3] = { "font", "fontClean", "fontWorld" };

    for (int i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s.fnt", kFontNames[i]);

        if (!parseFntFile(&bmfontTable(i), path, i)) {
            return false;
        }
    }

    return true;
}

// dispatch any triggered sounds from the queue to the audio engine
// reconstructed from FUN_100035cfc (getNext), FUN_100035de4 (getGain),
// FUN_100035e80 (getPitch), and the iOS GameViewController dispatch loop
void Game::dispatchSounds() {
    // SE gain context: picked from gb.seVolume when gameplay is active,
    // else the global SE volume. mirrors FUN_100035cfc:
    //   pfVar1 = &globalSeVolume;
    //   if (gb->visible) pfVar1 = &gb->seVolume;
    //   if (*pfVar1 <= 0.05f) skip all sounds (DAT_10005a1d4 = 0.05)
    float seGain = (boardPtr() && boardPtr()->visible)
        ? boardPtr()->seVolume
        : globalSeVolume();

    if (seGain <= 0.05f) {
        // all sounds muted, just clear the queue
        soundQueue.resetCursor();

        while (soundQueue.getNextTriggered() != SOUND_QUEUE_NONE) {
        }

        return;
    }

    // clamp SE gain to 0-1 (from FUN_100035de4)
    if (seGain > 1.0f) {
        seGain = 1.0f;
    }

    soundQueue.resetCursor();
    int slot;

    while ((slot = soundQueue.getNextTriggered()) != SOUND_QUEUE_NONE) {

        // every triggered slot draws variant + pitch off stream 0 and plays,
        // matching the binary's unconditional per-slot calls (GameViewController
        // ::update), keeping the cosmetic RNG stream in sync. slot is always
        // < NUM_SOUND_SLOTS (82 = sentinel).

        // pick a random variant handle (FUN_100035ef4, stream 0). a non-empty
        // slot always draws, even with a single variant; an empty slot yields an
        // invalid handle, which playSound ignores.
        int handle = -1;

        if (sSoundVariantCount[slot] > 0) {
            int variant = rngInt(0, sSoundVariantCount[slot] - 1, 0);
            handle = sSoundHandles[slot][variant];
        }

        // gain (FUN_100035de4): clamp(seGain,0,1)^2 * clamp(tableGain * 1.3, 0, 1).
        // seGain is already clamped to (0.05, 1.0] above.
        float scaled = sSoundGains[slot] * SOUND_GAIN_SCALE;

        if (scaled > 1.0f) {
            scaled = 1.0f;
        }

        if (scaled < 0.0f) {
            scaled = 0.0f;
        }

        float gain = seGain * seGain * scaled;

        // random pitch 0.85 to 1.15 (FUN_100035e80, DAT_10005a1dc/e0, stream 0).
        float pitch = rngFloat(0.85f, 1.15f, 0);

        AudioEngine::playSound(handle, gain, pitch);
    }
}

// resetFade (crossfade out), then queue the track for whichever top-level
// subsystem is now visible: title -> track 0, score panel -> silence, gameplay
// -> one random track (1..N-1). this fires on screen changes; level-to-level
// changes within gameplay crossfade via GameBoard::initLevelContent.
void Game::syncMusic() {
    musicController.resetFade();

    if (scorePanel_ != nullptr && scorePanel_->visible) {
        // post-run score panel up -> no track pushed, fades to silence.
    } else if (titleMenu_.visible) {
        musicController.setTrack(0);
    } else if (boardPtr_ != nullptr && boardPtr_->visible) {
        // one random gameplay track; the level a player resumes or starts on
        // gets its own track, and each subsequent level crossfades to a new one.
        musicController.setTrack(rngInt(1, MUSIC_TRACK_COUNT - 1, 0));
    }

    musicController.applyToAudio();
}

void Game::setVirtualHeight(float vh) {
    sVirtualHeight = vh;
}

// reconstructed from FUN_100045250
void Game::init() {
    time_t t = time(nullptr);

    // seed all 5 LCG streams with `time + 1 + i`, then advance each once
    // (FUN_10005708c + FUN_1000570a8). matches FUN_100045250's prologue.
    // without this every session draws the same sequence from the default
    // seed of 999, producing identical item rolls / stat jitter / sound
    // pitches every game. the level loader's per-level seed table
    // re-seeds these later for deterministic layouts.
    for (uint32_t i = 0; i < 5; ++i) {
        rngSeed((int)t + 1 + (int)i, i);
        rngAdvance(i);
    }

    gameState() = 0;
    inputState() = 0;

    // (globalSeVolume / globalBgmVolume defaults are set in Game::create
    // before SaveSystem::load; see save_buffers.h. FUN_100045250 does
    // not touch them.)

    // create menu system (allocated separately, pointer stored in data[])
    boardPtr() = GameBoard::create();

    if (boardPtr()) {
        SDL_Log("GameBoard created, not yet visible (Level::generate() will do that)");
    }

    // restore order mirrors FUN_100045250 exactly: save buffers first, THEN
    // initVisuals so the title-menu indicator gates see the restored state.

    // FUN_100054338, pull the saved unlocks back into shop, prune each
    // unlocked ID from the matching pool. shopSaveBuffer was populated
    // by SaveSystem::load earlier in startup; if no save existed it
    // still holds value-initialized empties, and restoreFromSave just
    // seeds the default pools.
    shop().restoreFromSave(shopSaveBuffer());

    // FUN_100038004, push the saved XferEntries back into the 3
    // ScoreHistory lists.
    leaderboardMenu().restoreFromSave(leaderboardSaveBuffer());

    // FUN_10004e6ac, pull saved AchievementTracker state back: states[]
    // + counters + 3 unique-set progress + damagedThisRun. mirrors the
    // binary's call sequence in FUN_100045250 right after
    // LeaderboardMenu::restoreFromSave.
    achievementTracker().restoreFromSave(achievementSaveBuffer());

    // init board (title screen + game board). binary calls
    //   FUN_10004ad80(titleMenu,
    //                 0 < shop.keys,
    //                 FUN_100037e88(leaderboardMenu));
    // gating the shop / leaderboard / achievements indicator buttons on
    // (1) any saved keys and (2) any saved leaderboard entries.
    titleMenu().initVisuals(shop().keys > 0, scoreHistory().hasAnyEntries());

    // sync music (FUN_10004535c). titleMenu just opened -> setTrack(0). the
    // first Game::update frame establishes the volume from gb.bgmVolume.
    syncMusic();

    SDL_Log("Game::init() complete");
}

// reconstructed from FUN_100045410
void Game::update(float dt) {

    // clamp dt to [0.0, 0.1]: FUN_1000570d4(dt, 0.0, DAT_10005a4a8=0.1)
    if (dt > MAX_DELTA_TIME) {
        dt = MAX_DELTA_TIME;
    } else if (dt < 0.0f) {
        dt = 0.0f;
    }

    // touch state machine (from decompilation)
    int ns = inputState();

    if (ns == 3 || ns == 4) {
        inputState() = 0;
    } else if (ns == 2 && gameState() == 0) {
        inputState() = 1;
    } else if (ns == 2 || gameState() == 1) {
        inputState() = 2;
    }

    // update sound queue first: clears last frame's play flags, decrements delays.
    // new sounds triggered after this point (by game logic) will be dispatched
    // at the end of this function.
    soundQueue.update(dt);

    // update music fading
    musicController.update(dt);

    // setTargetVolume from gb.bgmVolume (PauseMenu's row[1] slider) when
    // gameplay is active, else the global BGM volume. matches
    // FUN_100045410's FUN_100008800 call:
    //   pfVar6 = &globalBgmVolume;
    //   if (gb->visible) pfVar6 = &gb->bgmVolume;
    //   MusicController::setTargetVolume(*pfVar6);
    musicController.setTargetVolume(
        (boardPtr() && boardPtr()->visible)
            ? boardPtr()->bgmVolume
            : globalBgmVolume());

    // update overlay transition animation
    overlay().update(dt);

    // update title menu (from FUN_100045410 decompilation)
    // interactable = overlay not visible (piVar3 = overlay at 0x2E178)
    if (titleMenu().visible) {
        bool boardInteractable = !overlay().isVisible();
        titleMenu().update(dt, boardInteractable, &soundQueue);

        // FUN_100045410: when titleMenu is visible and overlay isn't running,
        // check the four button click flags in priority order. each opens
        // the transition overlay anchored at that button's posY; the
        // overlay-complete switch then dispatches the matching case.
        if (boardInteractable) {

            if (titleMenu().startButtonClicked) {
                titleMenu().startButtonClicked = false;

                // T = hasSavedRun ? 1 : 0. hasSavedRun is the slot-0
                // loader's `hasSavedRun = blobLength` write
                // (FUN_10004762c). non-zero -> valid "sav" blob was
                // deserialized at startup -> case 1 (resume via
                // FUN_100016b18). zero -> no save -> case 0 (new run via
                // character select). until task H/G ports FUN_100016b18
                // + wires the SaveSystem loader, this stays 0 and Start
                // always picks T=0.
                const int target = (hasSavedRun() != 0) ? 1 : 0;
                transitionTarget() = target;

                overlay().start(titleMenu().mainOverlayObj.quad.posY);
                SDL_Log("Game: startButton -> T=%d (hasSavedRun=%llu)",
                        target, (unsigned long long)hasSavedRun());
            }
            else if (titleMenu().shopClicked) {

                transitionTarget() = 2;
                overlay().start(titleMenu().shopObjA.quad.posY);
                SDL_Log("Game: indicator1 -> T=2 (shop)");
            }
            else if (titleMenu().leaderboardClicked) {

                transitionTarget() = 3;
                overlay().start(titleMenu().leaderboardObjA.quad.posY);
                SDL_Log("Game: indicator2 -> T=3 (leaderboard)");
            }
            else if (titleMenu().achievementsClicked) {

                transitionTarget() = 4;
                overlay().start(titleMenu().achievementsObjA.quad.posY);
                SDL_Log("Game: indicator3 -> T=4 (achievements)");
            }
        }
    }

    // update world/character selection (from FUN_100045410 decompilation)
    // from decompilation: only runs when overlay is not visible
    if (world().visible && !overlay().isVisible()) {

        if (sCharSelectExiting) {
            // back-button exit: fade the char-select content to black, then
            // fire the curtain once it's fully black. its close phase is
            // invisible over the black screen, so only the case-10 swap +
            // curtain-OPEN on the title shows (music crossfading back up).
            if (world().tickFadeOut(dt)) {
                sCharSelectExiting = false;
                transitionTarget() = kBackWorldToTitle;
                overlay().start(titleMenu().mainOverlayObj.quad.posY);
            }
        } else {
            world().update(dt, &soundQueue);
        }

        // if a character was selected, generate the game level
        // from decompilation: checks characterSelected (byte 2) and menu not visible
        if (world().characterSelected && (boardPtr() == nullptr || !boardPtr()->visible)) {
            world().characterSelected = false;

            if (boardPtr()) {
                // from decompilation: FUN_1000161fc(*plVar1, param_2[0x6446], param_2[0x6447], ...)
                // these are world().selectedCharType and world().worldIndex
                int characterType = world().selectedCharType;
                int difficultyIndex = (int)world().worldIndex;

                // Level::generate (FUN_1000161fc). binary signature is
                //   (gb, char, diff, &shop.snagPool, &shop.eventPool);
                // the two filter sets gate which snags / events are
                // eligible to spawn this run.
                boardPtr()->initLevel(characterType, difficultyIndex,
                                      shop().snagPool, shop().eventPool);

                // post-Level::generate tail (tutorialFlag / stashedDifficulty).
                // tutorialFlag is reset because Level::generate consumed it
                // (= seeded gb.tutorialFlag); stashedDifficulty captures the
                // difficulty this run was launched at, consumed by case 0
                // of the transition switch on title-return.
                tutorialFlag()      = false;
                stashedDifficulty() = difficultyIndex;

                // sync music (FUN_10004535c). board.visible -> push tracks
                // 1..N-1 (gameplay layered stack).
                syncMusic();

                SDL_Log("Game: Level::generate called (char=%d, diff=%d)", characterType, difficultyIndex);
            }
        }
    }

    // GameBoard update (FUN_100018ac8). binary's FUN_100045410 also calls
    // FUN_1000184e4 (= handleIdle) before the update when state == 0, to
    // kick a freshly-inited GameBoard out of state-0 purgatory into idle.
    //
    // the binary gates this whole block (handleIdle + gameBoardUpdate + the
    // run-end score/exit/snapshot decision) on !scorePanel.visible and on the
    // transition-active flag (param_2[0xb8cc] == overlay.isOpening()): the board
    // freezes behind the post-run ScorePanel and while a transition curtain is
    // closing the screen. the !scorePanel.visible half is load-bearing: without
    // it, the frame after a forfeit re-enters the block, the rising-edge
    // score-open `if` is now false (panel visible), and control falls through to
    // the dirty->save=1 snapshot branch, which re-saves and overwrites the
    // run-end save-invalidate (save=2). that let a finished run be resumed
    // whenever something kept gb.dirty set after the forfeit (the tutorial
    // cascade does exactly that via fireHint).
    {
        GameBoard* gb = boardPtr();

        if (gb && gb->visible && (!scorePanel() || !scorePanel()->visible)
            && !overlay().isOpening()) {

            if (gb->state == 0 && !world().visible) {
                gb->handleIdle();
            }

            // second arg = binary's gameBoardUpdate param_2 = hardcoded 1.0f.
            // carries the posY-seed value that nemesis-spawn / nemesis-step
            // forward to placeOnHexGrid / setNemesisPanTarget.
            gb->update(dt, 1.0f);

            // ---- scoreRequested branch (= player died or tapped Forfeit,
            //      "show me my score") ----
            //
            // mirrors FUN_100045410's outer `else` clause on the GameBoard.scoreRequested
            // byte (= scoreRequested):
            //   - pick the keys-earned count from per-difficulty / per-state
            //     tables (DAT_10005a4d0/4dc/4e8) when state < 5, else fall
            //     back to the per-difficulty cap (easy=3, normal=6, hard=8)
            //   - open ScorePanel with the GameBoard.totalTurnCount run-stat array
            //   - call FUN_100037d3c (stat-history insert) -> returns rank
            //     for setResultRankVisual
            //   - call FUN_100054254 + music sync
            //
            // we gate on !scorePanel->visible so the open only fires once on
            // the rising edge. once F3 / F4 / F5 land the full cleanup chain
            // gets wired alongside.
            if (gb->scoreRequested && scorePanel() && !scorePanel()->visible) {

                // keys-earned lookup. binary reads GameBoard.worldLevelIndex = worldLevelIndex
                // (= 1-indexed level the player reached this run; bumps in
                // initLevelContent). tables at DAT_10005a4d0/4dc/4e8 hold 3
                // ints per difficulty for levels 2/3/4; level 1 yields 0
                // keys (the `iVar16 < 2` fallback); level 5+ uses the
                // per-difficulty cap.
                //   easy:   levels 2/3/4 -> {0, 1, 2},  5+ -> 3
                //   normal: levels 2/3/4 -> {1, 2, 4},  5+ -> 6
                //   hard:   levels 2/3/4 -> {1, 3, 5},  5+ -> 8
                static constexpr int kKeysEasy[3]   = { 0, 1, 2 };
                static constexpr int kKeysNormal[3] = { 1, 2, 4 };
                static constexpr int kKeysHard[3]   = { 1, 3, 5 };

                int keys           = 0;
                int levelReached   = gb->worldLevelIndex;
                int worldIdx       = static_cast<int>(gb->worldIndex);

                if (levelReached >= 2) {

                    if (worldIdx == 2) {
                        keys = (levelReached <= 4)
                            ? kKeysHard[levelReached - 2] : 8;
                    } else if (worldIdx == 1) {
                        keys = (levelReached <= 4)
                            ? kKeysNormal[levelReached - 2] : 6;
                    } else {
                        keys = (levelReached <= 4)
                            ? kKeysEasy[levelReached - 2] : 3;
                    }
                }
// testing shop menu
#ifdef DEBUG_FAST_CTRL_ENABLED
                keys = 5;
#endif

                // GameBoard.totalTurnCount is the 7-int run-stat array consumed by the score
                // panel's open. fields in order: totalTurnCount,
                // worldLevelIndex, snagsDefeated, specialSnagsDefeated,
                // levelsGained, itemsFound, eventsFired.
                const int* counts = &gb->totalTurnCount;
                scorePanel()->open(counts, keys);

                // FUN_100037d3c. insert this run into the per-difficulty
                // stat-history and pass the returned rank to the score
                // panel's result-rank visual (medal / trophy). param order
                // matches the binary call site in FUN_100045410:
                //   tracker, worldIndex, playerSystem.characterIndex,
                //   levelsGained, itemsFound,
                //   worldLevelIndex - 1 (worlds), totalTurnCount (turns),
                //   totalScore
                const int rank = scoreHistory().insertEntry(
                    static_cast<uint32_t>(worldIdx),
                    static_cast<uint32_t>(gb->playerSystem.characterIndex),
                    static_cast<uint32_t>(counts[4]),    // levelsGained (GameBoard.levelsGained)
                    static_cast<uint32_t>(counts[5]),    // itemsFound (GameBoard.itemsFound)
                    static_cast<uint32_t>(gb->worldLevelIndex - 1),
                    static_cast<uint32_t>(counts[0]),    // totalTurnCount (GameBoard.totalTurnCount)
                    scorePanel()->totalScore);

                scorePanel()->setResultRankVisual(rank);

                // FUN_100054254. accumulate the run's keys-earned count
                // into the persistent shop balance. setting dirty here
                // signals the unlock-reward transition (= Shop::dirtyXfer,
                // FUN_100054298).
                shop().addKeys(keys);

                // invalidate the mid-run resume save (= FUN_100045410's
                // `param_2[0xb8d0] = 2`, i.e. saveSlot0Dirty = 2). the run is
                // over, so the snapshot must be deleted, otherwise the next
                // Start tap would resume this dead run straight into the
                // game-over screen. encodeSavedGame's dirty==2 branch clears
                // hasSavedRun + deletes the slot-0 file on the next flush.
                saveSlot0Dirty() = 2;

                // FUN_10004535c. scorePanel just became visible, so syncMusic
                // skips all setTrack branches -> music fades to silence.
                syncMusic();

                SDL_Log("Game: scoreRequested -> ScorePanel.open "
                        "(keys=%d, difficulty=%d, levelReached=%d)",
                        keys, worldIdx, levelReached);
            }

            // ---- exitRequested branch (= PauseMenu "Main Menu" tap) ----
            //
            // mirrors FUN_100045410's inner else on (!scoreRequested &&
            // exitRequested): write transitionTarget = 5 and start the
            // overlay anim. case 5 (already handled in the switch below)
            // closes gb and returns to TitleMenu without opening the
            // ScorePanel; the player chose "Main Menu" mid-run, no score.
            //
            // overlay.start arg = tabs[0].label.topY + pauseMenu.anchorY.
            // binary calls getLeftX on tabs[0].label and consumes the s1
            // (topY) component of the paired return; Ghidra collapses the
            // call into an unused-result invocation.
            else if (gb->exitRequested && !overlay().isVisible()) {
                transitionTarget() = 5;
                overlay().start(gb->pauseMenu.tabs[0].label.topY
                              + gb->pauseMenu.anchorY);
                SDL_Log("Game: exitRequested -> T=5 (Main Menu, no score)");
            }

            // ---- mid-run snapshot trigger (= FUN_1000269b8 call site) ----
            //
            // mirrors FUN_100045410's innermost else: when neither
            // scoreRequested nor exitRequested is set, and gb.dirty is
            // pending, and the game state is idle (state 1), and no
            // event-choice panel is up, pack the live GameBoard run-state
            // into the slot-0 GameSnapshot and flag the save framework.
            // dirtyXferSnapshot clears gb.dirty on entry, so each
            // qualifying frame fires at most one save build.
            else if (gb->dirty
                     && gb->state == 1
                     && !gb->eventChoicePanel.visible) {
                saveSlot0Dirty() = 1;
                gb->dirtyXferSnapshot(gameSnapshot());
            }

            // FUN_100026d00, slot-1 dirty + volume xfer gb -> global.
            if (gb->saveNewSettings) {
                saveSlot1Dirty()    = true;
                gb->saveNewSettings = false;
                globalSeVolume()    = gb->seVolume;
                globalBgmVolume()   = gb->bgmVolume;
            }
        }
    }

    // post-run score panel update + tap-to-dismiss -> T=6 set site.
    if (scorePanel() && scorePanel()->visible) {
        scorePanel()->update(dt);

        if (scorePanel()->closeRequested && !overlay().isVisible()) {
            transitionTarget() = 6;
            overlay().start(scorePanel()->titleAnchorY);
            SDL_Log("Game: scorePanel.closeRequested -> T=6");
        }
    }

    // T=7/8/9 set sites: fire when the shop / leaderboard / achievements
    // sub-screens flag closeRequested. all three reach the title-return
    // tail (cases 7/8/9 in the switch). timer for all three is touchY,
    // matching the binary's lookup of touchY_ through the global
    // game ptr (DAT_10007e868).
    // shop per-frame tick (FUN_1000525f4). drives the unlock-reveal anim
    // state machine + input hit-tests while visible. binary calls this
    // unconditionally inside Game::update's switch; we gate on visible
    // so the closed shop doesn't tick its decay loops.
    if (shop().visible) {
        shop().update(dt, 1.0f);
    }

    // shop dirty xfer (FUN_100054298). when Shop::addKeys or Shop::update
    // flagged shop.dirty (= player earned keys mid-run), push the new
    // state into the persistent save buffer. binary call site is in
    // FUN_100045410 right after Shop::update.
    if (shop().dirty) {
        shopSaveDirty() = true;
        shop().dirtyXfer(shopSaveBuffer());
    }

    // leaderboard menu (FUN_100037b34). per-frame touch input handler
    // while visible. binary calls unconditionally in FUN_100045410's
    // leaderboardMenu.visible-gated branch.
    if (leaderboardMenu().visible) {
        leaderboardMenu().update(touchX(), touchY());
    }

    // leaderboard dirty xfer (FUN_100037ebc). when ScoreHistory::insertEntry
    // flagged leaderboardMenu.dirty (= a run just landed on the boards),
    // stage every entry into the Game-level xfer vector. binary call site
    // is in FUN_100045410 right after FUN_100037b34. dirty-byte gate set
    // immediately before the call.
    if (leaderboardMenu().dirty) {
        leaderboardSaveDirty() = true;
        leaderboardMenu().dirtyXfer(leaderboardSaveBuffer());
    }

    // achievements menu (FUN_10005888c). per-frame touch + scroll inertia +
    // tile fade-in animation. binary calls in FUN_100045410's
    // achievementsMenu.visible-gated branch with dt = s0.
    if (achievementsMenu().visible) {
        achievementsMenu().update(dt);
    }

    // achievement tracker dirty xfer (FUN_10004e404). when any tracker
    // fire site flagged dirty (increment / markShown / notePlayerDamaged
    // / beginSession), filter-stage the persistable state into the
    // Game-level save buffer. binary's gate + xfer sequence at
    // FUN_100045410.
    if (achievementTracker().dirty) {
        saveSlot4Dirty() = true;
        achievementTracker().dirtyXfer(achievementSaveBuffer());
    }

    if (!overlay().isVisible()) {

        if (shop().visible && shop().closeRequested) {
            transitionTarget() = 7;
            overlay().start(touchY());
            SDL_Log("Game: shop.closeRequested -> T=7");
        }
        else if (leaderboardMenu().visible && leaderboardMenu().closeRequested) {
            transitionTarget() = 8;
            overlay().start(touchY());
            SDL_Log("Game: leaderboardMenu.closeRequested -> T=8");
        }
        else if (achievementsMenu().visible && achievementsMenu().closeRequested) {
            transitionTarget() = 9;
            overlay().start(touchY());
            SDL_Log("Game: achievementsMenu.closeRequested -> T=9");
        }
    }

    // the iOS Game::update's other per-frame work (tile icons, event menu,
    // stats, menus) all runs inside GameBoard::update, called above; there are
    // no separate top-level calls for it. the only unported top-level piece is
    // FUN_100045f6c, a debug frame counter writing to an undisplayed TextItem.

    // transitionTarget switch: fires once when the overlay completes its
    // open animation. each case mirrors a labeled branch in FUN_100045410's
    // post-switch dispatch (jump table at 0x100045f44). cases 5/6 share the
    // gameplay->title return tail (syncMusic); cases 7/8/9 share an
    // initVisuals-only title return (no syncMusic since the sub-screens
    // kept title music playing).
    if (overlay().isVisible() && overlay().isOpenComplete()) {
        const int target = transitionTarget();

        switch (target) {

            case 0: {
                // title -> character select. binary builds local set
                // {0..29} minus shop.facePool (= the unlocked-face IDs),
                // then hands it to World::generate which rng-pops one
                // ID per tile. binary's worldIndex source is the cached
                // stashedDifficulty_.
                std::set<int> unlockedFaces;

                for (int i = 0; i < 30; i++) {
                    unlockedFaces.insert(i);
                }

                for (int poolId : shop().facePool) {
                    unlockedFaces.erase(poolId);
                }

                titleMenu().visible = false;
                world().generate((uint32_t)stashedDifficulty(),
                                 unlockedFaces);
                syncMusic();
                SDL_Log("Game: T=0 -> character select (%d unlocked faces)",
                        (int)unlockedFaces.size());
                break;
            }

            case 1: {
                // title -> resume saved run. binary FUN_100045410's case 1
                // hides TitleMenu, calls FUN_100016b18(gb, snap, settings,
                // shop.snagPool, shop.eventPool), then jumps to syncMusic.
                // GameBoard::restoreFromSnapshot rebuilds the entire run from
                // the saved GameSnapshot (player / rack / placed tiles / HUD /
                // nemesis / hex map / reserve queue / RNG / exit chrome).
                titleMenu().visible = false;

                GameBoard* gb = boardPtr();

                if (gb) {
                    gb->restoreFromSnapshot(gameSnapshot(),
                                            shop().snagPool,
                                            shop().eventPool);
                }

                syncMusic();
                SDL_Log("Game: T=1 -> resume saved run");
                break;
            }

            case 2: {
                // title -> shop. Shop::open sets up row TextItems + key
                // icons + initial state; Shop::draw / Shop::update render and
                // run it while visible.
                titleMenu().visible = false;
                shop().open();
                // binary does not syncMusic here; shop inherits title music.
                SDL_Log("Game: T=2 -> shop");
                break;
            }

            case 3: {
                // title -> leaderboard. LeaderboardMenu::open ports
                // FUN_100037118, builds the 3-difficulty rank display from
                // ScoreHistory. binary does not syncMusic here; leaderboard
                // inherits title music.
                titleMenu().visible = false;
                leaderboardMenu().open();
                SDL_Log("Game: T=3 -> leaderboard");
                break;
            }

            case 4: {
                // title -> achievements. AchievementsMenu::open ports
                // FUN_100058410's second half: clears flags, rebuilds the
                // sortedDisplay map from AchievementTracker state, primes
                // each tile's progress visuals. binary does not syncMusic
                // here; achievements inherit title music.
                titleMenu().visible = false;
                achievementsMenu().open();
                SDL_Log("Game: T=4 -> achievements");
                break;
            }

            case 6: {
                // scorePanel tap-to-dismiss -> close panel, then fall
                // through to case 5 (close board + title return).
                if (scorePanel()) {
                    scorePanel()->visible = false;
                }
                [[fallthrough]];
            }
            case 5: {
                // gameplay -> title return (exitRequested or post-panel
                // dismiss). bring titleMenu back with the indicator hints
                // (shop badge if keys > 0, leaderboard / achievements
                // badges if any run completed). syncMusic drops from
                // gameplay tracks back to title's track 0.
                if (boardPtr()) {
                    boardPtr()->visible = false;
                }
                titleMenu().initVisuals(shop().keys > 0,
                                     scoreHistory().hasAnyEntries());
                syncMusic();
                SDL_Log("Game: T=%d -> title return (from gameplay)", target);
                break;
            }

            case 7: {
                // shop close -> title return. no syncMusic (shop inherits
                // title music).
                shop().visible = false;
                titleMenu().initVisuals(shop().keys > 0,
                                     scoreHistory().hasAnyEntries());
                SDL_Log("Game: T=7 -> title return (from shop)");
                break;
            }

            case 8: {
                // leaderboard close -> title return. no syncMusic.
                leaderboardMenu().visible = false;
                titleMenu().initVisuals(shop().keys > 0,
                                     scoreHistory().hasAnyEntries());
                SDL_Log("Game: T=8 -> title return (from leaderboard)");
                break;
            }

            case 9: {
                // achievements close -> title return. no syncMusic.
                achievementsMenu().visible = false;
                titleMenu().initVisuals(shop().keys > 0,
                                     scoreHistory().hasAnyEntries());
                SDL_Log("Game: T=9 -> title return (from achievements)");
                break;
            }

            case kBackWorldToTitle: {
                // back-button-only: character select -> title (reverse of
                // case 0). the char-select has already faded to black by the
                // time this fires, so hiding it is invisible; the curtain
                // then OPENS on the restored title. syncMusic crossfades the
                // music from silent back up to the title track.
                world().visible = false;
                titleMenu().initVisuals(shop().keys > 0,
                                     scoreHistory().hasAnyEntries());
                syncMusic();
                SDL_Log("Game: T=10 -> title return (from character select)");
                break;
            }

            default:
                SDL_Log("Game: unknown transitionTarget %d", target);
                break;
        }

        // start the close animation + play the transition-close sound
        // (FUN_100045410's tail: bl 0x100010740 + soundQueue 3).
        overlay().reset();
        soundQueue.trigger(3);
    }

    // apply music state changes.
    // in the ios version, GameViewController reads the music controller state
    // each frame and applies it to SoundEngine. we replicate that here.
    musicController.applyToAudio();

    // dispatch any sounds triggered this frame
    dispatchSounds();

    // commit state
    gameState() = inputState();
}

// reconstructed from FUN_100046000
void Game::draw() {
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();

    // draw subsystems in order (from FUN_100046000 decompilation):

    // 1. menu (pointer)
    if (boardPtr() && boardPtr()->visible) {
        boardPtr()->draw();
    }

    // 2. world/character selection
    if (world().visible) {
        world().draw();
    }

    // 3. shop. drawn between world and TitleMenu per
    // FUN_100046000 ordering.
    if (shop().visible) {
        shop().draw();
    }

    // 4. leaderboard menu. FUN_100037c28.
    if (leaderboardMenu().visible) {
        leaderboardMenu().draw();
    }

    // 5. achievements menu. FUN_100058eb4.
    if (achievementsMenu().visible) {
        achievementsMenu().draw();
    }

    // 6. title screen
    if (titleMenu().visible) {
        titleMenu().draw();
    }

    // 7. score panel (pointer). drawn after the title menu
    // so it overlays the gameplay scene at run-end. matches FUN_100046000's
    // visible-byte check on the scorePanel pointer.
    if (scorePanel() && scorePanel()->visible) {
        scorePanel()->draw();
    }

    // 8. overlay (from decompilation)
    if (overlay().isVisible()) {
        overlay().draw();
    }

    // 9. letterbox bars (from FUN_100046000)
    float sf = scaleFactor();

    if (sf > 0.0f) {
        bindTexture(0);
        letterboxQuad1_.draw();
        letterboxQuad2_.draw();
    }
}

// reconstructed from FUN_10004612c
void Game::touchBegan(float normX, float normY) {
    inputState() = 2;

    float sf = scaleFactor();
    touchX() = (sf + sf + 1.0f) * normX - sf;
    touchY() = sVirtualHeight * normY;
}

// reconstructed from FUN_100046168
void Game::touchMoved(float normX, float normY) {
    float sf = scaleFactor();
    touchX() = (sf + sf + 1.0f) * normX - sf;
    touchY() = sVirtualHeight * normY;
}

// reconstructed from FUN_10004619c. just sets inputState = 3 (released).
// the per-frame Game::update / GameBoard::update chain runs the touch
// state machine; release dispatch happens inside GameBoard::update's
// dispatchHexAndRackTouch (FUN_10001ae10), which calls queryReleaseTouch.
// we don't dispatch here directly.
void Game::touchEnded() {
    inputState() = 3;
}

// Android back button. see game.h for the routing summary. no binary
// equivalent; iOS has no back button. returns true only to quit the app.
bool Game::handleBackPressed() {
    // never interrupt a screen-transition curtain.
    if (overlay().isVisible()) {
        return false;
    }

    // post-run results showing -> absorb (the score panel has its own
    // tap-to-continue; back does nothing here).
    if (scorePanel() && scorePanel()->visible) {
        return false;
    }

    // in-game -> delegate to the board's front-to-back in-game routing.
    GameBoard* gb = boardPtr();

    if (gb && gb->visible) {
        gb->handleBackPressed();
        return false;
    }

    // sub-screens -> return to title via the same close signal their on-screen
    // back button raises (Game::update turns it into the T=7/8/9 transition).
    if (shop().visible) {
        // match the tap-close gate: the shop only closes while no unlock
        // animation is mid-flight (shop_menu.cpp gates the tap-off-close on
        // unlockAnimActive == 0). otherwise absorb the press.
        if (shop().unlockAnimActive == 0) {
            shop().closeRequested = true;
        }
        return false;
    }

    if (leaderboardMenu().visible) {
        leaderboardMenu().closeRequested = true;
        return false;
    }

    if (achievementsMenu().visible) {
        achievementsMenu().closeRequested = true;
        return false;
    }

    // character select -> return to title. mirror of the forward transition:
    // the char-select content fades out to black (the reverse of its fade-in),
    // then Game::update fires the curtain, whose close phase is invisible
    // over the already-black screen, so only the curtain-OPEN on the title
    // shows, with the music crossfading back up. ignore repeat presses while
    // the fade-out is already running.
    if (world().visible) {
        sCharSelectExiting = true;
        return false;
    }

    // title screen -> quit the app.
    if (titleMenu().visible) {
        return true;
    }

    // anything else (mid-state) -> absorb.
    return false;
}
