#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game/app_core.h"

namespace fgp {

// The "pipeline" minigame: a port of FGP.exe 0x00448410-0x0044b640.
// Implemented in app_pipeline.cpp.
class PipelineScene : protected virtual AppCore {
public:
    // The 0xdc-byte board records at 0x00678b28 (row-major, 10x8).
    struct PipelineCell {
        int terrain{};          // +0x00: mask digit, 1 bridge, 2 blocked
        int type{};             // +0x04: index into the pipe table 0x0049eba8
        int rotation{};         // +0x08
        int oil{};              // +0x0c
        std::array<std::array<int, 2>, 4> arms{}; // +0x10: per-direction fill
        bool path{};            // +0xd8: first piece after the well
        bool added{};           // the cell descriptor (+0x30) is in the scene
        std::string name;       // descriptor name, "%s-%d"
        bool oilDirty{};
        TexturePtr oilTexture;  // native composition of oil under the pipe
    };
    // The 0x18-byte route search nodes at 0x0067d030.
    struct PipelineNode {
        int g{};
        int h{};
        int f{};
        int open{};
        int parentColumn{-1};
        int parentRow{-1};
    };
    // The stack-linked chain of cells the oil came through (0x0044aa4d).
    struct PipelineVisit {
        const PipelineCell *cell{};
        const PipelineVisit *next{};
    };
    struct PipelinePieceImage {
        SurfacePtr surface;  // ARGB8888, green matte transparent
        TexturePtr texture;
    };
    struct PipelineClip {
        std::string name;
        bool added{};
        bool playing{};
        double started{};
        LiveAudioMixer::EffectHandle audio{LiveAudioMixer::kInvalidEffectHandle};
    };
    struct PipelineGame {
        std::array<PipelineCell, 80> cells;
        std::array<PipelineNode, 80> nodes;
        std::array<std::array<PipelinePieceImage, 4>, 10> pieces;
        std::array<TexturePtr, 5> levelTextures;
        std::array<TexturePtr, 2> startButtonTextures;
        std::array<TexturePtr, 2> turboButtonTextures;
        TexturePtr turboMeterTexture;
        Uint32 interiorColour{};
        std::map<std::string, DecodedMedia> media;
        std::map<std::string, TexturePtr> mediaTextures;

        // Globals of the original module.
        bool active{};              // 0x0067e938
        int level{};                // 0x0067e890
        int score{};                // 0x0067d858
        int buildStart{};           // 0x00678b24 (ms)
        int buildDuration{};        // 0x0067e93c (ms)
        int flowRate{};             // 0x00678b20 (units per second)
        int lastTick{};             // 0x0067ee88
        int minimumRoute{};         // 0x0067d85c
        int maximumRoute{};         // 0x0067e894
        int startColumn{};          // 0x0067d024
        int startRow{};             // 0x0067d028
        int endColumn{};            // 0x0067d00c
        int endRow{};               // 0x0067d010
        int startDirection{-1};     // 0x0067d020
        int endDirection{-1};       // 0x0067d008
        std::array<int, 7> inventory{}; // 0x0067cfec
        int pieceCount{};           // 0x0067d02c
        int held{-1};               // 0x0067cfe8
        int heldRotation{};         // 0x0067d018
        int heldX{};                // 0x004ddb8c, descriptor +0x6c
        int heldY{};                // 0x004ddb90, descriptor +0x70
        float turbo{};              // 0x0067d01c
        bool reachedEnd{};          // 0x0067d014

        // Scene descriptors the trace names.
        bool levelAdded{};
        std::string levelName{"level00"};
        PipelineClip well{"well"};
        PipelineClip lamp{"lamp"};
        PipelineClip cursor{"cursor"};
        PipelineClip running{"running-1"};
        bool heldAdded{};
        std::string heldName{"<unknown>"};
        std::vector<PipelineClip> clones;
        std::array<std::array<int, 2>, 80> solution{};

        // Native-only presentation state.
        bool entryAnnounced{};
        int wellX{};
        int wellY{};
        int lampX{};
        int lampY{};
        int cursorX{};
        int cursorY{};
        std::uint32_t pressedControl{};
        std::array<std::string, 7> inventoryText;
        std::string timerText;
        std::string scoreText;
        std::uint32_t randomState{};
        bool randomSeeded{};
        std::optional<std::uint32_t> fixedSeed;
    };

protected:
    PipelineGame loadPipelineGame(Package &package);
    void announcePipelineEntry();
    bool beginPipelineInteraction(int x, int y);
    void endPipelineInteraction(int x, int y);
    bool rotatePipelineAt(int x, int y);
    void movePipelinePointer(int x, int y);
    void updatePipeline(double elapsed);
    void renderPipeline(double elapsed);
    void continuePipelineAfterLevelMessage();
    void finishPipelineGameOver();
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        pipelineHotspotAt(int x, int y) const;
    // Developer control API for "pipeline:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executePipelineDeveloperCommand(std::string_view command);

    std::optional<PipelineGame> pipelineGame;

    // Reached by GameApp::startTournamentRound as well.
    void pipelineStartGame();
private:
    int pipelineClock();
    int pipelineRandom(int bound);
    PipelineCell &pipelineCell(int column, int row);
    void pipelineTraceAdd(std::string_view asset, int x, int y, int layer,
                          int hotspot = 0);
    void pipelineTraceRemove(std::string_view asset);
    void pipelineTraceStop(std::string_view asset);
    void pipelineSetupLevel();
    void pipelineLayTerrain();
    void pipelineRandomBuild();
    bool pipelineBuildRoute(bool randomEnds);
    void pipelineExpandNode(int column, int row, bool randomEnds);
    int pipelineRandomDirection(int column, int row);
    int pipelineExtractRoute();
    void pipelinePlaceRoutePiece(PipelineCell &cell, int nextColumn,
                                 int nextRow, int column, int row,
                                 int previousColumn, int previousRow);
    void pipelineComplicate();
    bool pipelineBendToCross(int column, int row);
    bool pipelineBendToTee(int column, int row);
    bool pipelineStraightToTee(int column, int row);
    bool pipelineCellFree(int column, int row);
    void pipelineShareInventory();
    void pipelineRedrawCell(PipelineCell &cell);
    void pipelineRedrawBoard();
    void pipelineSetInventoryText(int slot);
    void pipelineSetTurbo(float value);
    void pipelinePlaySound(std::string_view family, int variants);
    void pipelineRefreshHeld();
    void pipelineReturnHeld();
    void pipelineTakeFromInventory(int slot, int x, int y);
    bool pipelinePlaceHeld(int x, int y);
    bool pipelinePickUp(int x, int y);
    bool pipelineRotatePlaced(int x, int y);
    void pipelineHotspotPress(std::uint32_t hotspot, int x, int y);
    void pipelineControl(std::uint32_t control);
    void pipelineTick(int now);
    void pipelineUpdateTimer(int now);
    int pipelineFlowStep(float seconds);
    int pipelineFlow(int fromDirection, int column, int row, int amount,
                     const PipelineVisit *visited);
    void pipelineFillArms(int fromDirection, PipelineCell &cell, int amount);
    void pipelineLevelComplete();
    void pipelineGameOver();
    void pipelineAddClip(PipelineClip &clip, int x, int y, int layer);
    void pipelinePlayClip(PipelineClip &clip);
    void pipelineStopClip(PipelineClip &clip);
    void pipelineRemoveClip(PipelineClip &clip);
    void pipelineComposeOil(PipelineCell &cell);
};

} // namespace fgp
