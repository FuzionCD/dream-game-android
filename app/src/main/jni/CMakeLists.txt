cmake_minimum_required(VERSION 3.22.1)
project(Dream)

# sdl2
add_subdirectory(SDL2)

# sdl2_mixer (disable unused format decoders to keep build simple)
set(SDL2MIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_FLAC OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL2_mixer)

# openal-soft for sound effects (matching original iOS OpenAL usage)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(openal-soft)

# game library
add_library(main SHARED
    src/main.cpp
    src/platform.cpp
    src/audio_engine.cpp
    src/renderer.cpp
    src/quad.cpp
    src/game.cpp
    src/sound_queue.cpp
    src/music_controller.cpp
    src/overlay.cpp
        src/game_board.cpp
        src/title_menu.cpp
    src/world.cpp
    src/tile_collection.cpp
    src/color_tint.cpp
    src/nemesis_renderable.cpp
    src/gameplay_hud.cpp
    src/detail_panel.cpp
    src/dialog_panel.cpp
    src/portrait_table.cpp
    src/random.cpp
    src/movable_actor.cpp
        src/player_system.cpp
        src/item.cpp
        src/perk.cpp
        src/event_slot.cpp
        src/event_choice_panel.cpp
    src/tile_object.cpp
    src/tile_content.cpp
    src/snag_content.cpp
    src/stat_bars.cpp
    src/animation_controller.cpp
    src/hex_map.cpp
    src/achievement_tracker.cpp
    src/achievements_menu.cpp
    src/achievement_banner.cpp
    src/text_item.cpp
    src/level_up_panel.cpp
    src/item_choice_panel.cpp
    src/score_panel.cpp
    src/score_history.cpp
        src/shop_menu.cpp
    src/pause_menu.cpp
    src/label.cpp
    src/menu.cpp
    src/leaderboard_menu.cpp
    src/user_stats_panel.cpp
    src/save_system.cpp
)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_mixer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/openal-soft/include
)

target_link_libraries(main
    SDL2
    SDL2_mixer
    OpenAL
    GLESv1_CM
    log
)
