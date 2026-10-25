#include "game/app_morning.h"

#include <algorithm>
#include <cstdint>
#include <iostream>

namespace fgp {

namespace {

constexpr std::uint32_t kBirdGroup = 0x10U;
constexpr std::uint32_t kBirdLeaveTable = 0x000bdcf4U;

} // namespace

// Morning's callback 0x00425d60: BeginScene 0x00425e40, the shared click
// handler, the key handler 0x00425fb0, AnimFinished 0x00425ec0, timers
// 0x00425fc0 and the sequence-finished event 0x00425f90.
void MorningScene::buildMorningScene(SceneBuild &build) {
    morningBirdLeaveAt.reset();
    morningBirdLeaveBoundary.reset();
    morningLudvigAt.reset();

    // 0x00425e4a-0x00425e6f: bind group bit 0x10 with no dynamic hotspot,
    // birdbreath as category 1 (0x004bdce4) and birdidle as category 2
    // (0x004bdcec), then seed it (0x0040a880). Seeding leaves its special
    // deadline at 0, so the first birdbreath is followed by birdidle and the
    // deadline is re-armed after it (CharacterDeadlineRearm in the capture).
    build.addSceneCharacterGroup(0U, kBirdGroup, 0x000bdce4U, 0x000bdcecU);
    build.addSceneCharacterCategory4Table(kBirdGroup, kBirdLeaveTable);
    auto &birds = build.nextSceneCharacterGroups.back();
    birds.baseClipActivated = true;
    birds.category2DeadlineArmed = true;
    birds.category2DeadlineAt = 0.0;

    // 0x00425e74: squirrel then S07AN-SOL-001 as one sequence (0x004bdcd8).
    double sequenceOffset = 0.0;
    for (const std::uint32_t descriptorRva : {0x000bda38U, 0x000bdae0U}) {
        sequenceOffset += build.appendSceneDefinition(
            executableGameData.animationAtRva(descriptorRva),
            sequenceOffset);
    }
    build.nextSceneSequenceBlocksInput = true;

    // 0x00425e7e-0x00425e97: smoke, windmill and ripple are started on their
    // own; their descriptors set +0x4c, and none of them ever finishes in the
    // morning captures.
    for (const std::uint32_t descriptorRva :
         {0x000bd648U, 0x000bd6f0U, 0x000bd798U}) {
        build.addScenePlayer(descriptorRva, true);
    }
    // Ludvig's clip waits for timer 1 (0x00425fcc) and plays beside the
    // sequence: its conflict bit 2 does not meet SOL-001's bit 1.
    build.addScenePlayer(0x000bdb88U, false);

    // SOL-001's completion adds the bike bitmap (0x00425f19).
    build.addMenuBitmap(*build.nextPackage, "bike", 206, 325);
    build.nextSceneBitmapLayers.back().visible = false;

    std::cout << "Recovered morning setup (" << build.nextSceneClips.size()
              << " stages, " << sequenceOffset << " seconds)\n";
}

void MorningScene::handleMorningAnimationFinished(std::string_view name,
                                                  double completedAt) {
    if (name == "squirrel") {
        // 0x00425ee1-0x00425f03: timer 0 at now + 0x1770 ms, timer 1 at
        // now + 0x5aa0 ms.
        morningBirdLeaveAt = completedAt + 6.0;
        morningLudvigAt = completedAt + 23.2;
    } else if (name == "S07AN-SOL-001") {
        for (auto &bitmap : sceneBitmapLayers) {
            if (bitmap.name == "bike") {
                bitmap.visible = true;
            }
        }
        referenceTrace.emit("animation_add", {
            {"asset", "bike"}, {"x", -1}, {"y", -1}, {"layer", 0},
            {"hotspot", 0}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_start", {
            {"asset", "bike"}, {"x", -1}, {"y", -1}, {"layer", 0},
            {"loop", false}});
    } else if (name == "S07AN-LUD-001") {
        // 0x00425f2e: the RIDEDOWN question.
        showMorningRideConfirmation();
    }
}

void MorningScene::updateMorning(double now) {
    auto birds = std::find_if(
        sceneCharacterGroups.begin(), sceneCharacterGroups.end(),
        [](const SceneCharacterGroup &group) {
            return group.conflictMask == kBirdGroup;
        });
    if (birds == sceneCharacterGroups.end()) {
        return;
    }
    if (morningBirdLeaveAt && now >= *morningBirdLeaveAt) {
        // 0x0040a4e0 queues the request; the group takes it when its
        // current clip ends (birdleave follows a finished birdbreath).
        morningBirdLeaveAt.reset();
        double boundary = now;
        if (birds->baseClipPlaying) {
            if (const auto *clip = activeSceneCharacterClip(*birds)) {
                boundary = std::max(
                    now, birds->baseClipStartedAt + clip->duration);
            }
        }
        morningBirdLeaveBoundary = boundary;
    }
    if (morningBirdLeaveBoundary && now >= *morningBirdLeaveBoundary) {
        const double boundary = *morningBirdLeaveBoundary;
        morningBirdLeaveBoundary.reset();
        if (birds->baseClipPlaying) {
            if (const auto *clip = activeSceneCharacterClip(*birds)) {
                referenceTrace.emit("animation_stop", {
                    {"asset", clip->name}, {"reason", "completed"}});
            }
            birds->baseClipPlaying = false;
        }
        startSceneCharacterCategory4(kBirdGroup, kBirdLeaveTable, boundary);
    }
    if (birds->category4Playing) {
        // birdleave's completion disables the group (0x00425ecd calls
        // 0x0040a310 with bit 0x10); nothing replaces it.
        if (const auto *clip = activeSceneCharacterClip(*birds);
            clip != nullptr &&
            now - birds->baseClipStartedAt >= clip->duration) {
            referenceTrace.emit("animation_stop", {
                {"asset", clip->name}, {"reason", "completed"}});
            clearSceneCharacterCategory4(*birds);
            birds->baseClipPlaying = false;
            birds->enabled = false;
        }
    }
    if (morningLudvigAt && now >= *morningLudvigAt) {
        morningLudvigAt.reset();
        playSceneAnimationObject("S07AN-LUD-001");
    }
}

} // namespace fgp
