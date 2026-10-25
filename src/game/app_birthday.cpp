#include "game/app_birthday.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdexcept>

#include "assets/profile.h"
#include "game/activity_panel.h"
#include "media/smacker.h"

namespace fgp {
namespace {

// 0x004a97f0: the seven firework descriptors; 0x004a9330: the nine stars.
const auto kFireworkDescriptors = originalArray<std::uint32_t, 7>(0x4a97f0U);
const auto kStarDescriptors = originalArray<std::uint32_t, 9>(0x4a9330U);
constexpr std::uint32_t kImageBase = 0x00400000U;

// 0x004150a0: timer 0 (fireworks) first fires 3000 ms after entry and then
// every 8000 ms; timer 1 (stars) fires at entry and every 2000 ms.
constexpr double kFireworkDelay = 3.0;
constexpr double kFireworkPeriod = 8.0;
constexpr double kStarPeriod = 2.0;

// 0x00415141-0x0041515a: the greeting rectangle (200,350)-(600,550).
constexpr SDL_Rect kTextRect{200, 350, 400, 200};
constexpr int kTextLayer = 50;

std::tm localNow(std::time_t now) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    local = *std::localtime(&now);
#endif
    return local;
}

} // namespace

void BirthdayScene::loadBirthdayGame(std::vector<std::string> names) {
    BirthdayGame game;
    game.names = std::move(names);
    const auto fill = [this](auto &clips, const auto &descriptors,
                             int layerBase) {
        for (std::size_t index = 0; index < clips.size(); ++index) {
            const auto definition = executableGameData.animationAtRva(
                descriptors[index] - kImageBase);
            auto &clip = clips[index];
            clip.name = definition.name;
            clip.x = definition.x;
            clip.y = definition.y;
            clip.keyed = definition.keyed;
            clip.layer = static_cast<int>(index) + layerBase;
        }
    };
    fill(game.fireworks, kFireworkDescriptors, 0);
    fill(game.stars, kStarDescriptors, -100);

    // 0x004152b0: "HI name", ", name" for each middle name, "AND name" for
    // the last, then EXCLAMATION, a newline and HAPPYBIRTHDAY.
    std::string greeting = language.get("birthday", "HI", "Hei");
    for (std::size_t index = 0; index < game.names.size(); ++index) {
        if (index == 0) {
            greeting += ' ';
        } else if (index + 1 < game.names.size()) {
            greeting += language.get("birthday", "COMMA", ", ");
        } else {
            greeting += language.get("birthday", "AND", " og ");
        }
        greeting += game.names[index];
    }
    greeting += language.get("birthday", "EXCLAMATION", "!");
    greeting += '\n';
    greeting += language.get("birthday", "HAPPYBIRTHDAY",
                             "Gratulerer med dagen!");

    // 0x00415120-0x0041529b: the lines wrapped to the rectangle's width in
    // Amerigo BT_18_ (0x004150d6), each centred horizontally (0x004124e0
    // flag 0x100), on a descriptor sized to the text and placed at the
    // bottom of the rectangle -- the capture's (200,492) for two lines.
    const auto lines = wrapText(menuTitleFont, greeting, kTextRect.w);
    const int lineHeight = menuTitleFont.height;
    const int height = lineHeight * static_cast<int>(lines.size());
    SurfacePtr surface = createSurface(kTextRect.w, height);
    for (std::size_t line = 0; line < lines.size(); ++line) {
        const int width = textWidth(menuTitleFont, lines[line]);
        drawFontText(surface.get(), menuTitleFont, lines[line],
                     (kTextRect.w - width) / 2,
                     static_cast<int>(line) * lineHeight);
    }
    game.textRect = SDL_Rect{kTextRect.x, kTextRect.y + kTextRect.h - height,
                             kTextRect.w, height};
    game.text = makeTexture(renderer, surface.get());
    SDL_SetTextureBlendMode(game.text.get(), SDL_BLENDMODE_BLEND);
    birthdayGame = std::move(game);
    std::cout << "Recovered birthday greeting for "
              << birthdayGame->names.size() << " profile(s)\n";
}

void BirthdayScene::startBirthdayClip(BirthdayClip &clip, double elapsed) {
    // SCENE_AddAnim(descriptor, -1, -1, layer) then SCENE_PlayAnim
    // (0x00412840, 0x004129d0).
    clip.clip.media = decodeSmackerStreamed(
        activeSceneAnimationData(clip.name), clip.name,
        clip.keyed ? SmackerKeying::TopLeftRgb565
                   : SmackerKeying::IndexZeroAndGreen,
        true, true);
    if (!clip.clip.media.videoFrames.empty()) {
        const auto &frame = clip.clip.media.videoFrames.metadata(0U);
        clip.clip.texture.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            frame.width, frame.height));
        if (!clip.clip.texture) {
            throw std::runtime_error(std::string("SDL_CreateTexture failed: ") +
                                     SDL_GetError());
        }
        SDL_SetTextureBlendMode(clip.clip.texture.get(), SDL_BLENDMODE_BLEND);
    }
    clip.active = true;
    clip.startedAt = elapsed;
    referenceTrace.emit("animation_add", {
        {"asset", clip.name}, {"x", -1}, {"y", -1}, {"layer", clip.layer},
        {"hotspot", 0}, {"conflict_mask", 0}});
    if (!clip.clip.media.audioSamples.empty()) {
        audioMixer.addEffect(clip.clip.media,
                             audioVolumes[0] * sceneAudioGains[0]);
        SDL_PauseAudioDevice(audioDevice, musicEnabled && !paused ? 0 : 1);
    }
}

void BirthdayScene::updateBirthday(double elapsed) {
    auto &game = *birthdayGame;
    if (!game.entered) {
        // 0x004150a0 adds the greeting after arming both timers.
        game.entered = true;
        game.fireworkDue = elapsed + kFireworkDelay;
        game.starDue = elapsed;
        referenceTrace.emit("animation_add", {
            {"asset", ""}, {"x", game.textRect.x}, {"y", game.textRect.y},
            {"layer", kTextLayer}, {"hotspot", 0}, {"conflict_mask", 0}});
    }
    if (game.leaving || dialogMode != DialogMode::None) return;

    // 0x004161d0: up to 100 random picks (0x0040c530) for a clip that is not
    // playing, then 0x00415680's first idle one; none idle, none starts.
    const auto pick = [this](auto &clips) -> BirthdayClip * {
        const int count = static_cast<int>(clips.size());
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto &clip = clips[static_cast<std::size_t>(
                std::uniform_int_distribution<int>(0, count - 1)(
                    randomEngine))];
            if (!clip.active) return &clip;
        }
        for (auto &clip : clips) {
            if (!clip.active) return &clip;
        }
        return nullptr;
    };
    // Event 0x111: timer 0 is 0x00415630, timer 1 0x004156c0. The service
    // routine fires a due timer once per pass and advances it by one period
    // (0x0040bf9c), so a stall is made up over the following frames rather
    // than in a burst; hence `if`, not `while`.
    if (elapsed >= game.fireworkDue) {
        game.fireworkDue += kFireworkPeriod;
        if (auto *clip = pick(game.fireworks)) startBirthdayClip(*clip, elapsed);
    }
    if (elapsed >= game.starDue) {
        game.starDue += kStarPeriod;
        if (auto *clip = pick(game.stars)) startBirthdayClip(*clip, elapsed);
    }
    // Event 0x110 (0x00415600): a finished clip is removed.
    for (auto *clips : {static_cast<BirthdayClip *>(game.fireworks.data()),
                        static_cast<BirthdayClip *>(game.stars.data())}) {
        const std::size_t count = clips == game.fireworks.data()
                                      ? game.fireworks.size()
                                      : game.stars.size();
        for (std::size_t index = 0; index < count; ++index) {
            auto &clip = clips[index];
            if (!clip.active ||
                elapsed - clip.startedAt <
                    originalSmackerCompletionDuration(clip.clip.media)) {
                continue;
            }
            clip.active = false;
            clip.clip = {};
            referenceTrace.emit("animation_stop", {
                {"asset", clip.name}, {"reason", "finished"}});
            referenceTrace.emit("animation_remove", {
                {"asset", clip.name}, {"reason", "removed"}});
        }
    }
}

void BirthdayScene::renderBirthday(double elapsed) {
    if (!birthdayGame) return;
    auto &game = *birthdayGame;
    std::vector<BirthdayClip *> clips;
    for (auto &clip : game.fireworks) {
        if (clip.active) clips.push_back(&clip);
    }
    for (auto &clip : game.stars) {
        if (clip.active) clips.push_back(&clip);
    }
    std::stable_sort(clips.begin(), clips.end(),
                     [](const auto *left, const auto *right) {
                         return left->layer < right->layer;
                     });
    for (auto *clip : clips) {
        auto &media = clip->clip.media;
        if (!clip->clip.texture || media.videoFrames.empty()) continue;
        // A dialog holds the original's main loop, so a clip keeps the frame
        // it had when the award opened.
        const double time = elapsed - clip->startedAt;
        const auto &frame =
            media.videoFrames[frameIndexAt(media, time, false)];
        uploadVideoFrame(clip->clip.texture.get(), frame);
        SDL_Rect destination{clip->x, clip->y, frame.width, frame.height};
        SDL_RenderCopy(renderer, clip->clip.texture.get(), nullptr,
                       &destination);
    }
    if (game.entered && game.text) {
        SDL_RenderCopy(renderer, game.text.get(), nullptr, &game.textRect);
    }
}

bool BirthdayScene::handleBirthdayKey(SDL_Keycode key) {
    if (!birthdayGame || key != SDLK_SPACE) return false;
    // 0x00418980: VK_SPACE jumps to 0x0040cc30, the parent scene (menu).
    leaveBirthday();
    return true;
}

void BirthdayScene::leaveBirthday() {
    auto &game = *birthdayGame;
    if (game.leaving) return;
    game.leaving = true;
    // 0x0040cd70 traces EnterScene for the parent before the old scene's
    // 0x104 handler runs, so the menu's scene change precedes the awards
    // (capture 2026-10-18: menu at 14766 ms, award backdrop at 14797 ms).
    referenceTrace.emit("scene_change", {{"old_scene", "birthday"},
                                         {"scene", "menu"},
                                         {"entry_mode", 0}});
    menuSceneChangeTraced = true;

    // 0x004154a0: for each matched profile, read the time stored at the last
    // celebration (type 0x402, key 0, scene "birthday"), store the current
    // time, and award a present unless the last one fell on today's day of
    // the month in this or a later year (0x00415562-0x00415572).
    const std::time_t now = std::time(nullptr);
    const std::tm today = localNow(now);
    for (const auto &name : game.names) {
        const fs::path path = profileDirectory / (name + ".pfl");
        try {
            const std::uint32_t stored =
                profileSceneRecordU32(path, "birthday", 0x402U, 0U)
                    .value_or(0U);
            setProfileSceneRecordU32(path, "birthday", 0x402U, 0U,
                                     static_cast<std::uint32_t>(now));
            const std::tm last = localNow(static_cast<std::time_t>(stored));
            if (today.tm_mday != last.tm_mday ||
                today.tm_year > last.tm_year) {
                game.pendingAwards.push_back(name);
            }
        } catch (const std::exception &error) {
            std::cerr << "Could not update birthday record in " << path
                      << ": " << error.what() << '\n';
        }
    }
    continueBirthdayLeave();
}

bool BirthdayScene::continueBirthdayLeave() {
    if (!birthdayGame || !birthdayGame->leaving) return false;
    auto &game = *birthdayGame;
    while (!game.pendingAwards.empty()) {
        const std::string name = game.pendingAwards.front();
        game.pendingAwards.erase(game.pendingAwards.begin());
        // 0x00415574-0x004155a9: 0x0041bb20(0, AWARD_TITLE, AWARD_DESC with
        // the name, one part, 0). 0x0041b900 grants the next car part not
        // yet owned. UNCERTAIN: with all 62 owned, 0x0041b9d0's category-6
        // fallback (see Chess) is not ported and no dialog opens.
        const fs::path path = profileDirectory / (name + ".pfl");
        std::uint32_t part = 0;
        try {
            const std::uint32_t owned = profileCarPartCount(path);
            if (owned < 62U) part = awardMedalCarPart(path);
        } catch (const std::exception &error) {
            std::cerr << "Could not award birthday part in " << path << ": "
                      << error.what() << '\n';
        }
        if (part == 0U) continue;
        std::string number = std::to_string(part);
        if (number.size() < 2) number.insert(number.begin(), '0');
        const std::string format = language.get(
            "birthday", "AWARD_DESC", "Her er vår presang til deg %s på dagen!");
        std::vector<char> description(format.size() + name.size() + 1);
        std::snprintf(description.data(), description.size(), format.c_str(),
                      name.c_str());
        showAwardText("bitmap\\award\\part_" + number + ".bmp",
                      language.get("birthday", "AWARD_TITLE", "Gave"),
                      description.data());
        return true;
    }
    enterMenu();
    return true;
}

} // namespace fgp
