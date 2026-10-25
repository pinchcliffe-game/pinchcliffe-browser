#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/story_descriptors.h"

namespace fgp {

// Build-a-Car workshop (scene "buildacar"): its state and logic.
// Implemented in app_buildacar.cpp, a port of FGP.exe 0x00423210-0x00424640.
class BuildacarScene : protected virtual StoryDescriptors {
protected:
    // One shared push button (0x00406ed0): images by state (0 normal,
    // 1 pressed, 2 disabled) on its descriptor at layer 1.
    struct BuildacarButton {
        int id{};
        int state{};
        bool pressed{};
        std::array<std::string, 3> images;
        StoryAnim *anim{};
    };
    struct BuildacarPart {
        std::uint32_t number{};
        SDL_Rect menuBounds{};
        SDL_Rect targetBounds{};
        int dependency{-1};
        int variantGroup{-1};
    };
    struct BuildacarStage {
        std::uint32_t firstPart{};
        std::vector<BuildacarPart> parts;
    };
    struct BuildacarWorkshop {
        StoryStage stage;
        std::array<BuildacarStage, 5> stages;
        std::array<StoryAnim *, 14> menuAnims{};   // 0x005b7410
        std::array<StoryAnim *, 14> placedAnims{}; // 0x005b7e08
        StoryAnim *mousePart{};                    // 0x004b1f10
        StoryAnim *backdrop{};                     // 0x004b1fb8
        StoryAnim *frame{};                        // 0x004b0f80
        StoryAnim *iltempo{};                      // 0x004b1028
        StoryAnim *fanfare{};                      // 0x004b1178
        StoryAnim *blocker{};                      // 0x004a61b0
        std::array<StoryAnim *, 5> narrations{};   // 0x004b1568
        std::array<std::vector<StoryAnim *>, 5> stageClips; // 0x004b1ef8
        // STAGE, STAGEPRCNT, TOTALPRCNT and MISSING (0x00423c50).
        std::array<StoryAnim *, 4> texts{};
        std::array<BuildacarButton, 4> buttons; // ids 5 hint .. 8 demolish
        BitmapFont font;                        // 0x005b6280
        // Record 0x402 key 0 at 0x005b7d48 (0x00423386).
        std::uint32_t collectedParts{};  // 0x005b7d48
        std::uint32_t placedParts{};     // 0x005b7d4c, highest placed
        std::uint32_t currentStage{};    // 0x005b7d50
        std::uint32_t completedStages{}; // 0x005b7d54
        std::uint32_t stageMask{};       // 0x005b7d58
        int selectedPart{-1};            // 0x005b61d0
        std::optional<std::size_t> draggingPart;
        // The hint flash timer 0x005b72b0 (period 200 ms) and its count
        // 0x005b7360.
        bool hintTimer{};
        double hintDue{};
        int hintPart{};
        int hintCount{};
        bool hasProfile{};
        bool initialized{};
        double clockOffset{};
    };

    bool playBuildacarStageNarration(std::uint32_t stage);
    BuildacarWorkshop loadBuildacarWorkshop(Package &package);
    bool beginBuildacarInteraction(int x, int y);
    void endBuildacarInteraction(int x, int y);
    bool buildacarInteractionActive() const;
    void updateBuildacar(double elapsed);
    void demolishBuildacar();
    bool handleBuildacarKey(SDL_Keycode key);
    // Draws every workshop descriptor in layer order. Called from
    // renderFrame at the workshop's layer.
    void renderBuildacarWorkshop(double elapsed);
    // Developer control API for "buildacar:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeBuildacarDeveloperCommand(std::string_view command);
    // Adds this scene's contents while loadScene builds it.
    void buildBuildacarScene(
        SceneBuild &build,
        std::optional<BuildacarWorkshop> &nextBuildacarWorkshop);

    std::uint32_t buildacarNarrationProgress{0};
    std::optional<BuildacarWorkshop> buildacarWorkshop;

private:
    double buildacarClock();
    void initializeBuildacar(double now);
    bool buildacarPartPlaced(std::uint32_t stage, int part) const;
    bool buildacarStageComplete() const;
    int buildacarAvailableParts() const;
    BuildacarButton *buildacarButton(int id);
    void refreshBuildacarButton(BuildacarButton &button);
    void setBuildacarButtonState(int id, int state);
    void createBuildacarButton(int id, int x, int y,
                               const std::array<std::string, 3> &images);
    void setBuildacarText(std::size_t index, std::string text);
    void selectBuildacarStage(std::uint32_t stage, double now);
    void addBuildacarPlacedPart(int part);
    void refreshBuildacarVariants(int part);
    std::string buildacarPartName(int part) const;
    void updateBuildacarProgress();
    void stopBuildacarNarrations(double now);
    void startBuildacarHint(int part, double now);
    void pressBuildacarHint(double now);
    void changeBuildacarStage(int delta, double now);
    void dropBuildacarPart(std::size_t part, int x, int y, double now);
    void awardBuildacarMilestones(std::uint32_t from, std::uint32_t count);
    void startBuildacarFinale(double now);
    void endBuildacarFinale(double now);
    void controlBuildacar(int id, double now);
    void persistBuildacarState();
};

} // namespace fgp
