#include "game/app_buildacar.h"
#include "assets/original_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#include "assets/profile.h"
#include "util/binary.h"

// Build-a-Car is a port of the scene handler at FGP.exe 0x00423210, which
// switches on event - 0x103 through the byte table at 0x00423334 and the
// jump table at 0x00423308: 0x103 enters (0x00423350), 0x104 leaves
// (0x00423dd0), 0x105 presses (0x004240a0), 0x107 opens the navigation
// popup (0x004245c0), 0x10b drops a dragged part (0x00424210), 0x10c is a
// key (0x00424630), 0x110 an animation finishing (0x004245f0), 0x111 the
// hint timer (0x00424510), 0x116 a control (0x00423e40) and 0x11a a hover
// (0x004244d0). Every SCENE_AddAnim/RemoveAnim/StopAnim on its descriptors
// is mirrored in the reference trace. REVERSE_ENGINEERING.md
// ("Build-a-Car workshop") has the account.

namespace fgp {
namespace {

constexpr std::uint32_t kMenuParts = 0x005b7410U;
constexpr std::uint32_t kPlacedParts = 0x005b7e08U;
constexpr std::uint32_t kDescriptorSize = 0xa8U;
constexpr std::uint32_t kMousePart = 0x004b1f10U;
constexpr std::uint32_t kBackdrop = 0x004b1fb8U;
constexpr std::uint32_t kFrame = 0x004b0f80U;
constexpr std::uint32_t kIltempo = 0x004b1028U;
constexpr std::uint32_t kFanfare = 0x004b1178U;
constexpr std::uint32_t kBlocker = 0x004a61b0U;
// 0x004b1568: S10AN-NAR-001..005.
constexpr std::uint32_t kNarrations = 0x004b1220U;
// 0x004b1ef8: the stage clip tables, null for stage 2.
constexpr std::array<std::uint32_t, 5> kStageClipTables{{
    0x000b1eb0U, 0U, 0x000b1ec8U, 0x000b1ed4U, 0x000b1ee8U}};
// 0x00423c50: the four text controls 0x005b72b8, 0x005b7d60, 0x005b61d8
// and 0x005b7368 on the rectangles at 0x0049d5b8.
constexpr std::array<std::uint32_t, 4> kTextDescriptors{{
    0x005b72b8U, 0x005b7d60U, 0x005b61d8U, 0x005b7368U}};
const auto kTextRects = originalArray<SDL_Rect, 4>([](const OriginalData &source, std::size_t index) {
    const auto address = 0x0049d5b8U + static_cast<std::uint32_t>(index * 16);
    const auto r = source.read<std::array<int, 4>>(address);
    return SDL_Rect{r[0], r[1], r[2] - r[0], r[3] - r[1]};
});
constexpr std::size_t kStageText = 0;
constexpr std::size_t kStagePercentText = 1;
constexpr std::size_t kTotalPercentText = 2;
constexpr std::size_t kMissingText = 3;
constexpr int kTextLayer = 15;
constexpr int kTextFlags = 0x101; // centred both ways (0x004124e0)
constexpr int kButtonLayer = 1;   // 0x0040669f
constexpr int kHint = 5;
constexpr int kNext = 6;
constexpr int kPrevious = 7;
constexpr int kDemolish = 8;
constexpr std::uint32_t kAllParts = 62U;

std::string numbered(const char *format, std::uint32_t number) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), format, number);
    return buffer;
}

bool contains(const SDL_Rect &bounds, int x, int y) {
    const SDL_Point point{x, y};
    return SDL_PointInRect(&point, &bounds) == SDL_TRUE;
}

SDL_Rect toSdlRect(const ExecutableGameData::BuildacarRectangle &bounds) {
    return {bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top};
}

int ftolRound(float value) {
    // fadd 0.5 (0x0049c300) then _ftol (0x0048c214) truncates.
    return static_cast<int>(value + 0.5F);
}

} // namespace

BuildacarScene::BuildacarWorkshop BuildacarScene::loadBuildacarWorkshop(
    Package & /*package*/) {
    BuildacarWorkshop workshop;
    for (std::size_t index = 0; index < workshop.stages.size(); ++index) {
        const auto definition = executableGameData.buildacarStage(index);
        auto &stage = workshop.stages[index];
        stage.firstPart = definition.firstPart;
        for (const auto &partDefinition : definition.parts) {
            stage.parts.push_back({partDefinition.number,
                                   toSdlRect(partDefinition.menuBounds),
                                   toSdlRect(partDefinition.targetBounds),
                                   partDefinition.dependency,
                                   partDefinition.variantGroup});
        }
    }
    auto &stage = workshop.stage;
    // 0x00423460 and 0x004234d0 (through 0x00423480): fourteen keyed,
    // visible bitmap descriptors each; menu parts carry hotspots 100-113.
    for (std::uint32_t index = 0; index < 14U; ++index) {
        workshop.menuAnims[index] = &makeStoryAnim(
            stage, kMenuParts + index * kDescriptorSize, "", false, true,
            true, false, 0, 0, 100U + index);
        workshop.placedAnims[index] = &makeStoryAnim(
            stage, kPlacedParts + index * kDescriptorSize, "", false, true,
            true, false, 0, 0, 0U);
    }
    workshop.mousePart = &loadStoryAnim(stage, kMousePart);
    workshop.backdrop = &loadStoryAnim(stage, kBackdrop);
    workshop.frame = &loadStoryAnim(stage, kFrame);
    workshop.iltempo = &loadStoryAnim(stage, kIltempo);
    workshop.fanfare = &loadStoryAnim(stage, kFanfare);
    // UNCERTAIN: 0x00407160 builds the blocker's surface; native draws
    // nothing for it.
    workshop.blocker = &loadStoryAnim(stage, kBlocker);
    for (std::uint32_t index = 0; index < 5U; ++index) {
        workshop.narrations[index] = &loadStoryAnim(
            stage, kNarrations + index * kDescriptorSize, true);
        if (kStageClipTables[index] != 0U) {
            for (const auto &clip :
                 executableGameData.animationSequenceAtRva(
                     kStageClipTables[index])) {
                workshop.stageClips[index].push_back(
                    &loadStoryAnim(stage, clip.address));
            }
        }
    }
    for (std::size_t index = 0; index < kTextDescriptors.size(); ++index) {
        StoryAnim &text = makeStoryAnim(stage, kTextDescriptors[index], "",
                                        false, true, false, false,
                                        kTextRects[index].x,
                                        kTextRects[index].y, 0U);
        text.font = &dialogFont; // Amerigo BT_10_, 0x004233ab
        text.textRect = kTextRects[index];
        text.textFlags = kTextFlags;
        workshop.texts[index] = &text;
    }

    // 0x00423386 reads record 0x402 key 0 (20 bytes) into 0x005b7d48;
    // 0x00423395 then replaces the first word with the collected count
    // (0x0041b690 over category 0).
    if (profiles[profileIndex] != "No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(profilePath)) {
            workshop.hasProfile = true;
            const auto bytes = readBinaryFile(profilePath);
            workshop.collectedParts =
                std::min<std::uint32_t>(readLe32(bytes, 0x58U), kAllParts);
            if (const auto stored = profileBuildacarState(profilePath)) {
                workshop.placedParts = stored->placedParts;
                workshop.currentStage = stored->currentStage;
                workshop.completedStages = stored->completedStages;
                workshop.stageMask = stored->stageMask;
            }
        }
    }
    if (developmentMode) {
        workshop.collectedParts = kAllParts;
    }
    workshop.currentStage = std::min<std::uint32_t>(workshop.currentStage, 4U);
    std::cout << "Recovered Build-a-Car workshop ("
              << workshop.collectedParts << " collected, "
              << workshop.placedParts << " placed, stage "
              << workshop.currentStage + 1U << "/5) from 0x00423210\n";
    return workshop;
}

void BuildacarScene::buildBuildacarScene(
    SceneBuild &build,
    std::optional<BuildacarWorkshop> &nextBuildacarWorkshop) {
    // The workshop itself loads once the scene's packages are active
    // (loadBuildacarWorkshop); here only the narration stage is read.
    (void)build;
    nextBuildacarWorkshop.reset();
    buildacarNarrationProgress = 0;
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex] + ".pfl");
    if (profiles[profileIndex] != "No profile" && fs::exists(profilePath)) {
        buildacarNarrationProgress =
            profileSceneRecordU32(profilePath, "buildacar", 0x402U, 1U)
                .value_or(0U);
    }
}

double BuildacarScene::buildacarClock() {
    return sceneElapsedNow();
}

bool BuildacarScene::buildacarPartPlaced(std::uint32_t stage, int part) const {
    // 0x00423850 with 1 << part (0x004239d0).
    const auto &workshop = *buildacarWorkshop;
    if (workshop.completedStages > stage) {
        return true;
    }
    if (workshop.completedStages < stage || part < 0 || part >= 32) {
        return false;
    }
    const std::uint32_t bit = 1U << static_cast<unsigned>(part);
    return (workshop.stageMask & bit) == bit;
}

bool BuildacarScene::buildacarStageComplete() const {
    // 0x00423830: 0x00423850 with every part's bit.
    const auto &workshop = *buildacarWorkshop;
    const auto count = static_cast<unsigned>(
        workshop.stages[workshop.currentStage].parts.size());
    const std::uint32_t mask = (1U << count) - 1U;
    if (workshop.completedStages > workshop.currentStage) {
        return true;
    }
    if (workshop.completedStages < workshop.currentStage) {
        return false;
    }
    return (workshop.stageMask & mask) == mask;
}

int BuildacarScene::buildacarAvailableParts() const {
    // 0x004238fc-0x00423921: collected - first + 1, clamped to the stage.
    const auto &workshop = *buildacarWorkshop;
    const auto &stage = workshop.stages[workshop.currentStage];
    const int available = static_cast<int>(workshop.collectedParts) -
                          static_cast<int>(stage.firstPart) + 1;
    return std::clamp(available, 0, static_cast<int>(stage.parts.size()));
}

BuildacarScene::BuildacarButton *BuildacarScene::buildacarButton(int id) {
    for (auto &button : buildacarWorkshop->buttons) {
        if (button.id == id) {
            return &button;
        }
    }
    return nullptr;
}

void BuildacarScene::refreshBuildacarButton(BuildacarButton &button) {
    // 0x00406760: the state's image (0x00406810/0x00406840: state 1 shows
    // the pressed image only while pressed) replaces the descriptor's.
    const auto state = static_cast<std::size_t>(std::clamp(button.state, 0, 2));
    const std::string &name =
        state == 1U && !button.pressed ? button.images[0] : button.images[state];
    StoryAnim &anim = *button.anim;
    if (anim.active) {
        if (!name.empty() && anim.name == name) {
            return;
        }
        removeStoryAnim(anim, buildacarClock());
    }
    if (!name.empty()) {
        anim.name = name;
        addStoryAnim(buildacarWorkshop->stage, anim, -1, -1, kButtonLayer);
    }
}

void BuildacarScene::setBuildacarButtonState(int id, int state) {
    // 0x00406720 -> 0x00406740 refreshes only on a change.
    if (BuildacarButton *button = buildacarButton(id);
        button != nullptr && button->state != state) {
        button->state = state;
        refreshBuildacarButton(*button);
    }
}

void BuildacarScene::createBuildacarButton(
    int id, int x, int y, const std::array<std::string, 3> &images) {
    // 0x00406ed0: 0x00406540 creates the button only if the id is free;
    // either way 0x004068b0 stores each image and refreshes the button when
    // the image belongs to its current state.
    auto &workshop = *buildacarWorkshop;
    BuildacarButton *button = buildacarButton(id);
    if (button == nullptr) {
        button = &*std::find_if(workshop.buttons.begin(), workshop.buttons.end(),
                                [](const BuildacarButton &candidate) {
                                    return candidate.id == 0;
                                });
        button->id = id;
        button->state = 0;
        button->anim = &makeStoryAnim(
            workshop.stage, 0x00533688U + static_cast<std::uint32_t>(
                                              std::distance(
                                                  workshop.buttons.data(),
                                                  button) + 2) *
                                              0x278U + 0x1cU,
            "", false, true, true, false, x, y, static_cast<std::uint32_t>(id));
    }
    for (std::size_t index = 0; index < images.size(); ++index) {
        if (images[index].empty()) {
            continue;
        }
        button->images[index] = images[index];
        if (button->state == static_cast<int>(index)) {
            refreshBuildacarButton(*button);
        }
    }
}

void BuildacarScene::setBuildacarText(std::size_t index, std::string text) {
    // 0x004079c0 redraws the control's surface; the descriptor stays.
    buildacarWorkshop->texts[index]->text = std::move(text);
}

std::string BuildacarScene::buildacarPartName(int part) const {
    // 0x00423ab0: "part_%02d%s", with "b" (0x004b21fc) while the part's
    // variant group (0x004b0d6c) is not in the scene (0x00423b00).
    const auto &workshop = *buildacarWorkshop;
    const auto &stage = workshop.stages[workshop.currentStage];
    const auto &definition = stage.parts[static_cast<std::size_t>(part)];
    const bool variant =
        definition.variantGroup != -1 &&
        !workshop.placedAnims[static_cast<std::size_t>(definition.variantGroup)]
             ->active;
    return numbered("part_%02d", stage.firstPart + static_cast<std::uint32_t>(part)) +
           (variant ? "b" : "");
}

void BuildacarScene::addBuildacarPlacedPart(int part) {
    // 0x00423a70.
    auto &workshop = *buildacarWorkshop;
    StoryAnim &anim = *workshop.placedAnims[static_cast<std::size_t>(part)];
    anim.name = buildacarPartName(part);
    addStoryAnim(workshop.stage, anim, -1, -1, part + 5);
    refreshBuildacarVariants(part);
}

void BuildacarScene::refreshBuildacarVariants(int part) {
    // 0x00423b40: every placed part whose variant group is `part` is
    // re-added under its current name.
    auto &workshop = *buildacarWorkshop;
    const auto &parts = workshop.stages[workshop.currentStage].parts;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        StoryAnim &anim = *workshop.placedAnims[index];
        if (!anim.active || parts[index].variantGroup != part) {
            continue;
        }
        removeStoryAnim(anim, buildacarClock());
        anim.name = buildacarPartName(static_cast<int>(index));
        addStoryAnim(workshop.stage, anim, -1, -1, static_cast<int>(index) + 5);
    }
}

void BuildacarScene::updateBuildacarProgress() {
    // 0x004236c0: the parts in the scene (0x00423740), as a share of the
    // stage and, from the stage's first part, of all 62 (float 1/62 at
    // 0x0049d5f8, 100 at 0x0049cc68).
    auto &workshop = *buildacarWorkshop;
    const auto &stage = workshop.stages[workshop.currentStage];
    int inScene = 0;
    for (std::size_t index = 0; index < stage.parts.size(); ++index) {
        inScene += workshop.placedAnims[index]->active ? 1 : 0;
    }
    const float total = static_cast<float>(
        static_cast<int>(stage.firstPart) + inScene - 1) *
        (1.0F / 62.0F) * 100.0F;
    const float share = static_cast<float>(inScene) /
                        static_cast<float>(stage.parts.size()) * 100.0F;
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s: %3d %%",
                  language.get("buildacar", "STAGEPRCNT", "Dette nivået").c_str(),
                  ftolRound(share));
    setBuildacarText(kStagePercentText, buffer);
    std::snprintf(buffer, sizeof(buffer), "%s: %3d %%",
                  language.get("buildacar", "TOTALPRCNT", "Totalt").c_str(),
                  ftolRound(total));
    setBuildacarText(kTotalPercentText, buffer);
}

void BuildacarScene::stopBuildacarNarrations(double now) {
    // 0x00423640: every narration that is playing is removed.
    for (StoryAnim *narration : buildacarWorkshop->narrations) {
        if (narration->playing) {
            removeStoryAnim(*narration, now);
        }
    }
}

bool BuildacarScene::playBuildacarStageNarration(std::uint32_t stage) {
    // 0x004235e0: the stage's narration plays once, the first time the
    // stage is reached; record 0x402 key 1 holds the next stage to narrate.
    if (activeScene != "buildacar" || !buildacarWorkshop || stage >= 5U) {
        return false;
    }
    auto &workshop = *buildacarWorkshop;
    const double now = buildacarClock();
    stopBuildacarNarrations(now);
    if (stage < buildacarNarrationProgress) {
        return false;
    }
    addAndPlayStoryAnim(workshop.stage, *workshop.narrations[stage], now);
    buildacarNarrationProgress = stage + 1U;
    if (workshop.hasProfile) {
        setProfileSceneRecordU32(
            profileDirectory / (profiles[profileIndex] + ".pfl"), "buildacar",
            0x402U, 1U, buildacarNarrationProgress);
    }
    return true;
}

void BuildacarScene::selectBuildacarStage(std::uint32_t stage, double now) {
    // 0x00423500.
    auto &workshop = *buildacarWorkshop;
    // 0x00423bc0 removes the old stage's clips, 0x00423c10 plays the new
    // stage's (0x00412b90; they loop).
    for (StoryAnim *clip : workshop.stageClips[workshop.currentStage]) {
        if (clip->active) {
            removeStoryAnim(*clip, now);
        }
    }
    for (StoryAnim *clip : workshop.stageClips[stage]) {
        addAndPlayStoryAnim(workshop.stage, *clip, now);
    }
    workshop.currentStage = stage;
    workshop.selectedPart = -1;
    // 0x00423880: "stage%d" at layer -10.
    if (workshop.backdrop->active) {
        removeStoryAnim(*workshop.backdrop, now);
    }
    workshop.backdrop->name = numbered("stage%d", stage + 1U);
    addStoryAnim(workshop.stage, *workshop.backdrop, -1, -1, -10);
    // 0x004238d0: the collected menu parts not yet placed, at layer 0.
    const auto &parts = workshop.stages[stage].parts;
    for (StoryAnim *menu : workshop.menuAnims) {
        if (menu->active) {
            removeStoryAnim(*menu, now);
        }
    }
    const int available = buildacarAvailableParts();
    for (int index = 0; index < available; ++index) {
        StoryAnim &menu = *workshop.menuAnims[static_cast<std::size_t>(index)];
        const auto &part = parts[static_cast<std::size_t>(index)];
        menu.name = numbered("menu_part_%02d", part.number);
        menu.defaultX = part.menuBounds.x;
        menu.defaultY = part.menuBounds.y;
        if (!buildacarPartPlaced(stage, index)) {
            addStoryAnim(workshop.stage, menu, -1, -1, 0);
        }
    }
    // 0x004239f0: the placed parts at their targets.
    for (StoryAnim *placed : workshop.placedAnims) {
        if (placed->active) {
            removeStoryAnim(*placed, now);
        }
    }
    for (std::size_t index = 0; index < parts.size(); ++index) {
        StoryAnim &placed = *workshop.placedAnims[index];
        placed.defaultX = parts[index].targetBounds.x;
        placed.defaultY = parts[index].targetBounds.y;
        if (buildacarPartPlaced(stage, static_cast<int>(index))) {
            addBuildacarPlacedPart(static_cast<int>(index));
        }
    }
    // 0x00423670: "%s: %d / %d".
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s: %u / %d",
                  language.get("buildacar", "STAGE", "Nivå").c_str(),
                  stage + 1U, 5);
    setBuildacarText(kStageText, buffer);
    updateBuildacarProgress();
    playBuildacarStageNarration(stage);
    // 0x00423557-0x00423585: the last stage's Next is the car button.
    if (stage >= 4U) {
        createBuildacarButton(kNext, 747, 421, {"car_0", "car_1", "car_2"});
    } else {
        createBuildacarButton(kNext, 747, 421, {"next_0", "next_1", "next_2"});
    }
    const bool complete = buildacarStageComplete();
    setBuildacarButtonState(kHint, complete ? 2 : 0);
    setBuildacarButtonState(kPrevious, stage == 0U ? 2 : 0);
    setBuildacarButtonState(kNext, complete ? 0 : 2);
    persistBuildacarState();
}

void BuildacarScene::initializeBuildacar(double now) {
    // 0x00423350 from 0x004233c4, after the record and font are loaded.
    auto &workshop = *buildacarWorkshop;
    workshop.initialized = true;
    // 0x00423c50. UNCERTAIN: 0x00423c50 also writes a registry value
    // (0x0040b940, HKCU) and 0x004234d0 reads one; native keeps neither.
    for (StoryAnim *text : workshop.texts) {
        addStoryAnim(workshop.stage, *text, text->defaultX, text->defaultY,
                     kTextLayer);
    }
    createBuildacarButton(kHint, 0, 421, {"hint_0", "hint_1", "hint_2"});
    createBuildacarButton(kNext, 747, 421, {"next_0", "next_1", "next_2"});
    createBuildacarButton(kPrevious, 704, 428, {"prev_0", "prev_1", "prev_2"});
    createBuildacarButton(kDemolish, 53, 428, {"demolish_0", "demolish_1", ""});
    // 0x00423410: "%s: %2d" missing parts.
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s: %2u",
                  language.get("buildacar", "MISSING", "Manglende deler").c_str(),
                  kAllParts - workshop.collectedParts);
    setBuildacarText(kMissingText, buffer);
    selectBuildacarStage(workshop.currentStage, now);
}

void BuildacarScene::startBuildacarHint(int part, double now) {
    // 0x00423f60: flash the part every 200 ms (AddTimer at the clock with
    // period 200, id = part) and disable Hint meanwhile.
    auto &workshop = *buildacarWorkshop;
    workshop.placedAnims[static_cast<std::size_t>(part)]->name =
        buildacarPartName(part);
    workshop.hintCount = 0;
    workshop.hintTimer = true;
    workshop.hintDue = now;
    workshop.hintPart = part;
    setBuildacarButtonState(kHint, 2);
}

void BuildacarScene::pressBuildacarHint(double now) {
    // 0x00423ec0: the part last picked up, or its missing prerequisite;
    // otherwise 0x00423fd0's first part (or prerequisite) not in the scene.
    auto &workshop = *buildacarWorkshop;
    const auto &parts = workshop.stages[workshop.currentStage].parts;
    const int selected = workshop.selectedPart;
    if (selected >= 0 && selected < static_cast<int>(parts.size()) &&
        !workshop.placedAnims[static_cast<std::size_t>(selected)]->active) {
        const int dependency = parts[static_cast<std::size_t>(selected)].dependency;
        if (dependency != -1 &&
            !workshop.placedAnims[static_cast<std::size_t>(dependency)]->active) {
            startBuildacarHint(dependency, now);
        } else {
            startBuildacarHint(selected, now);
        }
        return;
    }
    const int available = buildacarAvailableParts();
    for (int index = 0; index < available; ++index) {
        const int dependency = parts[static_cast<std::size_t>(index)].dependency;
        if (dependency != -1 &&
            !workshop.placedAnims[static_cast<std::size_t>(dependency)]->active) {
            startBuildacarHint(dependency, now);
            return;
        }
        if (!workshop.placedAnims[static_cast<std::size_t>(index)]->active) {
            startBuildacarHint(index, now);
            return;
        }
    }
}

void BuildacarScene::changeBuildacarStage(int delta, double now) {
    // 0x00423e90: past the last stage the finale plays.
    auto &workshop = *buildacarWorkshop;
    int target = static_cast<int>(workshop.currentStage) + delta;
    if (target >= 5) {
        startBuildacarFinale(now);
        return;
    }
    target = std::max(target, 0);
    if (target != static_cast<int>(workshop.currentStage)) {
        selectBuildacarStage(static_cast<std::uint32_t>(target), now);
    }
}

void BuildacarScene::demolishBuildacar() {
    // 0x00424086: a confirmed DEMOLISH clears the progress words and
    // returns to stage 1; the highest placed part (0x005b7d4c) stays.
    if (!buildacarWorkshop) {
        return;
    }
    auto &workshop = *buildacarWorkshop;
    workshop.completedStages = 0U;
    workshop.stageMask = 0U;
    selectBuildacarStage(0U, buildacarClock());
}

void BuildacarScene::awardBuildacarMilestones(std::uint32_t from,
                                              std::uint32_t count) {
    // 0x00424420: placed-part milestones in the 16-byte table at
    // 0x004b0d80 (part, kind, two award words). Kind 20 is RACEGAME and
    // kind 21 CARUPGRADE, shown as buildacar:AWARD message boxes; the
    // other kinds go to the award wrapper 0x0041bb20.
    // UNCERTAIN: what 0x0041bb20 grants for those kinds; native only logs
    // them.
    const auto milestones = executableGameData.buildacarMilestones();
    for (std::uint32_t part = from; part < from + count; ++part) {
        const auto found = std::find_if(
            milestones.begin(), milestones.end(),
            [part](const auto &milestone) { return milestone.part == part; });
        if (found == milestones.end()) {
            continue;
        }
        if (found->kind == 20U || found->kind == 21U) {
            showMessage(language.get(
                "buildacar", found->kind == 20U ? "RACEGAME" : "CARUPGRADE",
                ""));
        } else {
            std::cout << "Build-a-Car milestone at part " << part
                      << " (kind " << found->kind
                      << ") is not granted by the native port yet\n";
        }
    }
}

void BuildacarScene::dropBuildacarPart(std::size_t part, int x, int y,
                                       double now) {
    // 0x00424210.
    auto &workshop = *buildacarWorkshop;
    const auto &definition = workshop.stages[workshop.currentStage].parts[part];
    StoryAnim &menu = *workshop.menuAnims[part];
    removeStoryAnim(*workshop.mousePart, now);
    if (!contains(definition.targetBounds, x, y)) {
        addStoryAnim(workshop.stage, menu, -1, -1, 0);
        playStoryClone(workshop.stage, "wrong", now);
        return;
    }
    const int dependency = definition.dependency;
    if (dependency != -1 &&
        !workshop.placedAnims[static_cast<std::size_t>(dependency)]->active) {
        addStoryAnim(workshop.stage, menu, -1, -1, 0);
        playStoryClone(workshop.stage, "notyet", now);
        startBuildacarHint(dependency, now);
        return;
    }
    // 0x004243c0.
    workshop.stageMask |= 1U << static_cast<unsigned>(part);
    addBuildacarPlacedPart(static_cast<int>(part));
    int inScene = 0;
    for (const StoryAnim *placed : workshop.placedAnims) {
        inScene += placed->active ? 1 : 0;
    }
    const auto highest = static_cast<std::uint32_t>(
        inScene + static_cast<int>(workshop.stages[workshop.currentStage].firstPart) - 1);
    if (highest > workshop.placedParts) {
        awardBuildacarMilestones(workshop.placedParts + 1U,
                                 highest - workshop.placedParts);
        workshop.placedParts = highest;
    }
    playStoryClone(workshop.stage, "right", now);
    updateBuildacarProgress();
    if (buildacarStageComplete()) {
        ++workshop.completedStages;
        workshop.stageMask = 0U;
        setBuildacarButtonState(kHint, 2);
        setBuildacarButtonState(kNext, 0);
    }
    persistBuildacarState();
}

void BuildacarScene::startBuildacarFinale(double now) {
    // 0x00423d50: unless it already plays, Il Tempo Gigante drives out of
    // its frame over the blocker, with fanfare2.
    auto &workshop = *buildacarWorkshop;
    if (workshop.iltempo->playing) {
        return;
    }
    stopBuildacarNarrations(now);
    // 0x00407110(0, 0x95): the blocker at layer 0x95.
    if (!workshop.blocker->active) {
        addStoryAnim(workshop.stage, *workshop.blocker, -1, -1, 0x95);
    }
    addStoryAnim(workshop.stage, *workshop.frame, -1, -1, 0x96);
    addStoryAnim(workshop.stage, *workshop.iltempo, -1, -1, 0x97);
    playStoryAnim(workshop.stage, *workshop.iltempo, now);
    addAndPlayStoryAnim(workshop.stage, *workshop.fanfare, now);
}

void BuildacarScene::endBuildacarFinale(double now) {
    // 0x004241d0: any press while it plays removes it again.
    auto &workshop = *buildacarWorkshop;
    removeStoryAnim(*workshop.frame, now);
    removeStoryAnim(*workshop.iltempo, now);
    removeStoryAnim(*workshop.fanfare, now);
    if (workshop.blocker->active) {
        removeStoryAnim(*workshop.blocker, now);
    }
}

void BuildacarScene::controlBuildacar(int id, double now) {
    // Control event 0x116, 0x00423e40.
    switch (id) {
    case kHint:
        pressBuildacarHint(now);
        break;
    case kNext:
        changeBuildacarStage(1, now);
        break;
    case kPrevious:
        changeBuildacarStage(-1, now);
        break;
    case kDemolish:
        // 0x00424060: interfaceh:WARNING / buildacar:DEMOLISH, yes/no.
        showMessage(language.get("buildacar", "DEMOLISH",
                                 "Ønsker du å demontere Il Tempo Gigante?"),
                    MessageKind::BuildacarDemolishConfirmation);
        break;
    default:
        break;
    }
}

bool BuildacarScene::beginBuildacarInteraction(int x, int y) {
    if (activeScene != "buildacar" || !buildacarWorkshop) {
        return false;
    }
    auto &workshop = *buildacarWorkshop;
    const double now = buildacarClock();
    // 0x004240a0.
    if (workshop.iltempo->playing) {
        endBuildacarFinale(now);
    }
    if (!workshop.hintTimer) {
        const auto &parts = workshop.stages[workshop.currentStage].parts;
        const int available = buildacarAvailableParts();
        for (int index = 0; index < available; ++index) {
            StoryAnim &menu = *workshop.menuAnims[static_cast<std::size_t>(index)];
            if (!contains(parts[static_cast<std::size_t>(index)].menuBounds, x, y) ||
                !menu.active) {
                continue;
            }
            workshop.selectedPart = index;
            removeStoryAnim(menu, now);
            StoryAnim &mouse = *workshop.mousePart;
            mouse.name = numbered("mouse_part_%02d",
                                  parts[static_cast<std::size_t>(index)].number);
            addStoryAnim(workshop.stage, mouse, x, y, 100);
            // 0x00412cf0 centres it on the pointer (untraced move).
            if (const StoryImage *image = storyImage(workshop.stage, mouse.name, true)) {
                mouse.x = x - image->width / 2;
                mouse.y = y - image->height / 2;
            }
            workshop.draggingPart = static_cast<std::size_t>(index);
            return true;
        }
    }
    // The shared button handler 0x00406b50 runs after the scene's: a
    // normal-state push button under the pointer shows its pressed image.
    for (auto &button : workshop.buttons) {
        if (button.id == 0 || button.state != 0 || !button.anim->active) {
            continue;
        }
        const StoryImage *image =
            storyImage(workshop.stage, button.anim->name, true);
        if (image != nullptr &&
            contains(SDL_Rect{button.anim->x, button.anim->y, image->width,
                              image->height},
                     x, y)) {
            button.pressed = true;
            setBuildacarButtonState(button.id, 1);
            return true;
        }
    }
    return false;
}

bool BuildacarScene::buildacarInteractionActive() const {
    if (!buildacarWorkshop) {
        return false;
    }
    if (buildacarWorkshop->draggingPart) {
        return true;
    }
    return std::any_of(buildacarWorkshop->buttons.begin(),
                       buildacarWorkshop->buttons.end(),
                       [](const BuildacarButton &button) {
                           return button.pressed;
                       });
}

void BuildacarScene::endBuildacarInteraction(int x, int y) {
    if (!buildacarWorkshop) {
        return;
    }
    auto &workshop = *buildacarWorkshop;
    const double now = buildacarClock();
    if (workshop.draggingPart) {
        // The engine drag (0x0040c1f0) posts 0x10b at the release point.
        const std::size_t part = *workshop.draggingPart;
        workshop.draggingPart.reset();
        dropBuildacarPart(part, x, y, now);
        return;
    }
    // 0x00406d40: a released push button returns to normal and posts 0x116
    // when the pointer is still on it.
    for (auto &button : workshop.buttons) {
        if (!button.pressed) {
            continue;
        }
        button.pressed = false;
        const int id = button.id;
        const StoryImage *image =
            storyImage(workshop.stage, button.anim->name, true);
        const bool inside =
            image != nullptr &&
            contains(SDL_Rect{button.anim->x, button.anim->y, image->width,
                              image->height},
                     x, y);
        setBuildacarButtonState(id, 0);
        if (inside) {
            controlBuildacar(id, now);
        }
        return;
    }
}

bool BuildacarScene::handleBuildacarKey(SDL_Keycode key) {
    // 0x00424630: Space removes the narration that is playing.
    if (!buildacarWorkshop || key != SDLK_SPACE) {
        return false;
    }
    stopBuildacarNarrations(buildacarClock());
    return true;
}

void BuildacarScene::updateBuildacar(double elapsed) {
    if (!buildacarWorkshop || pendingScene) {
        return;
    }
    auto &workshop = *buildacarWorkshop;
    if (!workshop.initialized) {
        initializeBuildacar(elapsed);
    }
    if (workshop.draggingPart) {
        // The engine drag moves the descriptor with the pointer (0x0040c300).
        StoryAnim &mouse = *workshop.mousePart;
        if (const StoryImage *image = storyImage(workshop.stage, mouse.name, true)) {
            mouse.x = mouseX - image->width / 2;
            mouse.y = mouseY - image->height / 2;
        }
    }
    // 0x004245f0: a finished narration is removed.
    updateStoryDescriptors(workshop.stage, elapsed, [&](StoryAnim &anim) {
        if (std::find(workshop.narrations.begin(), workshop.narrations.end(),
                      &anim) != workshop.narrations.end()) {
            removeStoryAnim(anim, elapsed);
        }
    });
    // 0x00424510: each 200 ms tick toggles the hinted part; the third
    // removal stops the timer (0x0040c500) and enables Hint again. One tick
    // per pass, advanced by one period (0x0040bf9c), so not `while`.
    if (workshop.hintTimer && workshop.hintDue <= elapsed) {
        workshop.hintDue += 0.2;
        const int part = workshop.hintPart;
        StoryAnim &placed = *workshop.placedAnims[static_cast<std::size_t>(part)];
        if (placed.active) {
            removeStoryAnim(placed, elapsed);
            refreshBuildacarVariants(part);
            ++workshop.hintCount;
        } else {
            addBuildacarPlacedPart(part);
        }
        if (workshop.hintCount >= 3) {
            workshop.hintTimer = false;
            setBuildacarButtonState(kHint, 0);
        }
    }
}

void BuildacarScene::renderBuildacarWorkshop(double elapsed) {
    if (!buildacarWorkshop) {
        return;
    }
    renderStoryDescriptors(buildacarWorkshop->stage, elapsed);
}

void BuildacarScene::persistBuildacarState() {
    // 0x00423de4 writes record 0x402 key 0 when the scene ends; native
    // writes it on every change instead, with the same value.
    const auto &workshop = *buildacarWorkshop;
    if (workshop.hasProfile) {
        setProfileBuildacarState(
            profileDirectory / (profiles[profileIndex] + ".pfl"),
            {workshop.collectedParts, workshop.placedParts,
             workshop.currentStage, workshop.completedStages,
             workshop.stageMask});
    }
    // 0x004245c0: TV Station opens once 13 parts have been placed.
    if (developmentMode || workshop.placedParts >= 13U) {
        storyNextScene = "tvstation";
    } else {
        storyNextScene.clear();
    }
}

bool BuildacarScene::executeBuildacarDeveloperCommand(std::string_view command) {
    constexpr std::string_view buildacarControlPrefix = "buildacar:";
    if (activeScene != "buildacar" || !buildacarWorkshop) {
        return false;
    }
    auto &workshop = *buildacarWorkshop;
    if (!workshop.initialized) {
        return false;
    }
    const double now = buildacarClock();
    const std::string_view control =
        command.substr(buildacarControlPrefix.size());
    constexpr std::string_view placePrefix = "place:";
    if (control.rfind(placePrefix, 0) == 0) {
        // Pick up part N and drop it on its target.
        const auto number = static_cast<std::uint32_t>(std::stoul(
            std::string(control.substr(placePrefix.size()))));
        const auto &parts = workshop.stages[workshop.currentStage].parts;
        const auto found = std::find_if(
            parts.begin(), parts.end(),
            [number](const BuildacarPart &part) { return part.number == number; });
        if (found == parts.end()) {
            return false;
        }
        const auto index =
            static_cast<std::size_t>(std::distance(parts.begin(), found));
        if (!workshop.menuAnims[index]->active ||
            !beginBuildacarInteraction(found->menuBounds.x + 1,
                                       found->menuBounds.y + 1)) {
            return false;
        }
        endBuildacarInteraction(found->targetBounds.x + 1,
                                found->targetBounds.y + 1);
    } else if (control == "hint") {
        controlBuildacar(kHint, now);
    } else if (control == "previous") {
        controlBuildacar(kPrevious, now);
    } else if (control == "next") {
        controlBuildacar(kNext, now);
    } else if (control == "demolish") {
        demolishBuildacar();
    } else if (control == "status") {
        std::cout << "Build-a-Car status: " << workshop.collectedParts
                  << " collected, " << workshop.placedParts
                  << " placed, stage " << workshop.currentStage + 1U
                  << "/5, " << workshop.completedStages << " completed\n";
    } else {
        throw std::runtime_error(
            "Unknown developer Build-a-Car control: " + std::string(control));
    }
    return true;
}

} // namespace fgp
