cmake_minimum_required(VERSION 3.24)

project(FGPNativePOC VERSION 0.1.0 LANGUAGES CXX)

# This application performs substantial per-pixel conversion while decoding
# the original Smacker media. A no-configuration single-config build should
# therefore be usable, rather than silently producing the 4-5x slower Debug
# decoder observed during scene-load profiling. Developers can still request
# Debug explicitly with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build (Debug, Release, RelWithDebInfo, MinSizeRel)"
        FORCE)
endif()

if(MSVC)
    # Link the C and C++ runtimes statically. MSVCP140, VCRUNTIME140 and
    # VCRUNTIME140_1 are the only DLLs the program imports that a stock Windows
    # 10/11 lacks and the build does not copy beside it, so a dynamic runtime
    # means the "MSVCP140.dll was not found" box on a machine without the
    # Visual C++ Redistributable.
    # SDL2 and FFmpeg are DLLs of their own and keep their runtimes.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(FGP_DEVELOPMENT_MODE
    "Unlock story scenes and enable development controls"
    ON
)

# Not in the original: open the Scene Index's subgame and activity tabs from
# the start, without opening the story pages or anything else development mode
# changes. The public browser build uses it (deploy/web/build.sh).
option(FGP_OPEN_ALL_GAMES
    "Open every minigame and activity in the Scene Index"
    OFF
)

if(EMSCRIPTEN)
    # Browser build: emcmake cmake -S . -B build-web -G Ninja
    # SDL2 comes from Emscripten's port; FFmpeg is the minimal static build
    # made by tools/web/build-ffmpeg-wasm.sh. Tests stay native-only.
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(FGP_FFMPEG_WASM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-wasm"
        CACHE PATH "Minimal FFmpeg built for WebAssembly")
    if(NOT EXISTS "${FGP_FFMPEG_WASM_ROOT}/lib/libavformat.a")
        message(FATAL_ERROR "Run tools/web/build-ffmpeg-wasm.sh first "
                            "(no FFmpeg in ${FGP_FFMPEG_WASM_ROOT})")
    endif()
    add_library(fgp_web_ffmpeg INTERFACE)
    target_include_directories(fgp_web_ffmpeg INTERFACE "${FGP_FFMPEG_WASM_ROOT}/include")
    target_link_libraries(fgp_web_ffmpeg INTERFACE
        "${FGP_FFMPEG_WASM_ROOT}/lib/libavformat.a"
        "${FGP_FFMPEG_WASM_ROOT}/lib/libavcodec.a"
        "${FGP_FFMPEG_WASM_ROOT}/lib/libswresample.a"
        "${FGP_FFMPEG_WASM_ROOT}/lib/libswscale.a"
        "${FGP_FFMPEG_WASM_ROOT}/lib/libavutil.a")
elseif(WIN32)
    # Self-contained dependency layout used by the Windows build:
    #   third_party/SDL2    — SDL2 VC development libraries
    #   third_party/ffmpeg  — FFmpeg win64 shared build (headers, import libs, DLLs)
    set(FGP_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
        "Directory holding SDL2 and FFmpeg development libraries")

    list(APPEND CMAKE_PREFIX_PATH "${FGP_THIRD_PARTY}/SDL2")
    find_package(SDL2 REQUIRED CONFIG)

    set(FGP_FFMPEG_ROOT "${FGP_THIRD_PARTY}/ffmpeg" CACHE PATH
        "FFmpeg shared development build root")

    function(fgp_import_ffmpeg name)
        file(GLOB _dll "${FGP_FFMPEG_ROOT}/bin/${name}-*.dll")
        list(LENGTH _dll _dll_count)
        if(NOT _dll_count EQUAL 1 OR NOT EXISTS "${FGP_FFMPEG_ROOT}/lib/${name}.lib")
            message(FATAL_ERROR
                "Expected exactly one ${name}-*.dll (found ${_dll_count}) and "
                "lib/${name}.lib under ${FGP_FFMPEG_ROOT}")
        endif()
        add_library(FFmpeg::${name} SHARED IMPORTED)
        set_target_properties(FFmpeg::${name} PROPERTIES
            IMPORTED_LOCATION "${_dll}"
            IMPORTED_IMPLIB "${FGP_FFMPEG_ROOT}/lib/${name}.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${FGP_FFMPEG_ROOT}/include"
        )
    endfunction()

    # src/media/smacker.cpp uses AVChannelLayout and swr_alloc_set_opts2,
    # which arrived in FFmpeg 5.1 (libavutil 57.24).
    if(NOT EXISTS "${FGP_FFMPEG_ROOT}/include/libavutil/version.h")
        message(FATAL_ERROR "No FFmpeg headers under ${FGP_FFMPEG_ROOT}/include")
    endif()
    file(STRINGS "${FGP_FFMPEG_ROOT}/include/libavutil/version.h" _avutil_version
        REGEX "^#define LIBAVUTIL_VERSION_(MAJOR|MINOR) +[0-9]+")
    string(REGEX REPLACE ".*MAJOR +([0-9]+).*" "\\1" _avutil_major "${_avutil_version}")
    string(REGEX REPLACE ".*MINOR +([0-9]+).*" "\\1" _avutil_minor "${_avutil_version}")
    if("${_avutil_major}.${_avutil_minor}" VERSION_LESS 57.24)
        message(FATAL_ERROR "FFmpeg 5.1 or newer is needed (libavutil 57.24); "
            "${FGP_FFMPEG_ROOT} has libavutil ${_avutil_major}.${_avutil_minor}")
    endif()

    fgp_import_ffmpeg(avformat)
    fgp_import_ffmpeg(avcodec)
    fgp_import_ffmpeg(avutil)
    fgp_import_ffmpeg(swresample)
    fgp_import_ffmpeg(swscale)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    # FFmpeg 5.1 or newer: src/media/smacker.cpp uses AVChannelLayout and
    # swr_alloc_set_opts2.
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil>=57.24
        libswresample>=4.5
        libswscale
    )
endif()

add_executable(FGPNativePOC
    src/main.cpp
    src/game/app.cpp
    src/game/app_audiopairs.cpp
    src/game/app_balloonhunt.cpp
    src/game/app_beemaze.cpp
    src/game/app_bugzzz.cpp
    src/game/app_butterfly.cpp
    src/game/app_buildacar.cpp
    src/game/app_colorfill.cpp
    src/game/app_buildabike.cpp
    src/game/app_lettersort.cpp
    src/game/app_mahjong.cpp
    src/game/mahjong_board.cpp
    src/game/mahjong_engine.cpp
    src/game/app_hopscotch.cpp
    src/game/app_draughts.cpp
    src/game/draughts_rules.cpp
    src/game/app_hustle.cpp
    src/game/app_chess.cpp
    src/game/chess_engine.cpp
    src/game/app_jigsaw.cpp
    src/game/app_sliding.cpp
    src/game/jigsaw_cut.cpp
    src/game/picture_library.cpp
    src/game/puzzle_image.cpp
    src/game/puzzle_stage.cpp
    src/game/app_birthday.cpp
    src/game/app_bouquet.cpp
    src/game/app_credits.cpp
    src/game/app_fence.cpp
    src/game/app_puzzle.cpp
    src/game/puzzle_gem_renderer.cpp
    src/game/puzzle_data.cpp
    src/game/app_mainscore.cpp
    src/game/activity_panel.cpp
    src/game/app_activitypage.cpp
    src/game/app_buttercol.cpp
    src/game/app_racemenu.cpp
    src/game/app_racing.cpp
    src/game/racing_data.cpp
    src/game/racing_physics.cpp
    src/game/racing_ai.cpp
    src/game/awards.cpp
    src/game/app_tournament.cpp
    src/game/app_mathlab.cpp
    src/game/app_minigame_recovery.cpp
    src/game/app_mountain.cpp
    src/game/app_pee.cpp
    src/game/app_pipeline.cpp
    src/game/app_sockdrawer.cpp
    src/game/app_textinvader.cpp
    src/game/app_trivia.cpp
    src/game/app_synonym.cpp
    src/game/app_whackamole.cpp
    src/game/app_wheelbarrow.cpp
    src/game/app_yard.cpp
    src/game/app_viewer.cpp
    src/game/app_viewers.cpp
    src/game/app_anaglyph.cpp
    src/game/app_gallery.cpp
    src/game/app_info.cpp
    src/game/app_movieplayer.cpp
    src/game/app_desk.cpp
    src/game/app_dialogs.cpp
    src/game/app_events.cpp
    src/game/app_hotspots.cpp
    src/game/app_menu.cpp
    src/game/app_render.cpp
    src/game/app_web_controls.cpp
    src/game/app_run.cpp
    src/game/app_scene.cpp
    src/game/app_intro.cpp
    src/game/app_house.cpp
    src/game/app_morning.cpp
    src/game/app_outtent.cpp
    src/game/app_tvstation.cpp
    src/game/app_goodbye.cpp
    src/game/story_descriptors.cpp
    src/game/app_outro.cpp
    src/game/app_town.cpp
    src/game/app_tvroom.cpp
    src/game/app_intent.cpp
    src/assets/fact_table.cpp
    src/assets/help_table.cpp
    src/assets/profile.cpp
    src/assets/score_tables.cpp
    src/game/menu_names.cpp
    src/media/audio.cpp
    src/media/media.cpp
    src/media/smacker.cpp
    src/platform/command_line.cpp
    src/platform/console.cpp
    src/platform/paths.cpp
    src/platform/leaderboard.cpp
    src/platform/remote_data.cpp
    src/platform/web_ui.cpp
    src/platform/web_audio.cpp
    src/platform/single_instance.cpp
    src/render/bitmap_font.cpp
    src/render/sdl_util.cpp
    src/util/binary.cpp
    src/util/reference_trace.cpp
    src/util/text.cpp
)

target_include_directories(FGPNativePOC PRIVATE src)
target_compile_features(FGPNativePOC PRIVATE cxx_std_20)
target_compile_definitions(FGPNativePOC PRIVATE
    FGP_DEVELOPMENT_MODE=$<BOOL:${FGP_DEVELOPMENT_MODE}>
    FGP_OPEN_ALL_GAMES=$<BOOL:${FGP_OPEN_ALL_GAMES}>
)

if(MSVC)
    target_compile_options(FGPNativePOC PRIVATE /W4 /utf-8)
else()
    target_compile_options(FGPNativePOC PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(FGPNativePOC PRIVATE
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        SDL2::SDL2
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swresample
        FFmpeg::swscale
    )
    # UTF-8 process code page and long paths; the file says why.
    target_sources(FGPNativePOC PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/FGPNativePOC.manifest")
    # A player's build is a windowed program with no console: the GUI
    # subsystem, and no developer console because development mode is off.
    # Development builds keep the console subsystem, so diagnostics print in
    # the terminal that started them; -DCMAKE_WIN32_EXECUTABLE=ON overrides.
    if(NOT FGP_DEVELOPMENT_MODE)
        set_target_properties(FGPNativePOC PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    # Copy the runtime DLLs next to the executable so it runs in place.
    add_custom_command(TARGET FGPNativePOC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:FGPNativePOC> $<TARGET_FILE_DIR:FGPNativePOC>
        COMMAND_EXPAND_LISTS
    )

    # A folder a player can be handed: `cmake --build build --target package`
    # writes build/FGPNative-<version>-win64.zip with the program, the DLLs it
    # needs, where to put the game's files, and the libraries' licences. It
    # holds none of the original game.
    install(TARGETS FGPNativePOC RUNTIME DESTINATION .)
    install(FILES $<TARGET_RUNTIME_DLLS:FGPNativePOC> DESTINATION .)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/README.txt"
        DESTINATION .)
    foreach(_library SDL2 ffmpeg)
        if(EXISTS "${FGP_THIRD_PARTY}/${_library}/LICENSE.txt")
            install(FILES "${FGP_THIRD_PARTY}/${_library}/LICENSE.txt"
                DESTINATION licences RENAME "${_library}-LICENSE.txt")
        endif()
    endforeach()
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "FGPNative-${PROJECT_VERSION}-win64")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    include(CPack)
elseif(EMSCRIPTEN)
    target_link_libraries(FGPNativePOC PRIVATE fgp_web_ffmpeg)
    target_compile_options(FGPNativePOC PRIVATE -sUSE_SDL=2 -fwasm-exceptions)
    add_custom_target(fgp_web_data
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/web/prepare_web_data.py"
            --out "${CMAKE_BINARY_DIR}"
        COMMENT "Preparing the engine-only browser site"
        VERBATIM)
    add_dependencies(FGPNativePOC fgp_web_data)
    target_link_options(FGPNativePOC PRIVATE
        -sUSE_SDL=2 -fwasm-exceptions
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=8388608
        -sFORCE_FILESYSTEM=1
        # Full-screen page that fits phones and starts the game (and its
        # audio) from a tap; noInitialRun needs callMain exported.
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/tools/web/shell.html"
        # Profiles live in IndexedDB (IDBFS) so they survive reloads; the page
        # mounts it before the game starts.
        -lidbfs.js
        -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV
        -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stringToNewUTF8)
    set_target_properties(FGPNativePOC PROPERTIES SUFFIX ".html"
        # Relink when the page template changes.
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/web/shell.html")
else()
    target_link_libraries(FGPNativePOC PRIVATE
        PkgConfig::SDL2
        PkgConfig::FFMPEG
    )
endif()

if(APPLE)
    set_target_properties(FGPNativePOC PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "FGP Native POC"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.pinchcliffe.browser"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(NODE_EXECUTABLE node REQUIRED)
    add_executable(FGPOriginalDataTest tests/test_original_data.cpp
        src/game/puzzle_data.cpp src/util/binary.cpp)
    target_include_directories(FGPOriginalDataTest PRIVATE src)
    target_compile_features(FGPOriginalDataTest PRIVATE cxx_std_20)
    add_test(NAME original-data COMMAND FGPOriginalDataTest)
    add_test(NAME web-publication COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_publication.py")
    add_test(NAME embedded-data COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_embedded_data.py")
    add_test(NAME leaderboard-server COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_leaderboard_server.py")
    add_test(NAME web-local-files COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_local_files.cjs")
    add_test(NAME web-disc-image COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_disc_image.cjs")
endif()
