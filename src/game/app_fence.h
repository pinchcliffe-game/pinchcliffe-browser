#pragma once

#include "game/app_core.h"

namespace fgp {

// The "fence" activity (Musikkgjerdet): the fence planks are a keyboard,
// the telephone wires a stave whose notes are birds. Implemented in
// app_fence.cpp; the scene callback is FGP.exe 0x00466250.
class FenceScene : protected virtual AppCore {
protected:
    // The state word at 0x006b6228.
    enum class FenceMode {
        BirdsArriving = 0, BirdsLeaving = 1, Free = 2, Play = 3, Record = 4,
    };
    // One entry of the note array at 0x006b6230: pitch 0-17 (A1 ... D3)
    // and duration 0-7 (quaver, quaver-dot ... semibreve-dot).
    struct FenceNote {
        int pitch{};
        int duration{};
    };
    // The tune: notes, count (0x006b8230), tempo (0x006b8234) and the name
    // buffer (0x006b8238). These are process globals in the original, so a
    // second visit starts with the previous visit's tune on the stave.
    struct FenceTune {
        std::vector<FenceNote> notes;
        int bpm{120};
        std::string name;
    };
    // A static descriptor: SCENE_AddAnim state (+0x7c) and the position and
    // layer it was added with.
    struct FenceSprite {
        std::string name;
        bool added{};
        int x{};
        int y{};
        int layer{};
    };
    // One bird descriptor of the array at 0x006b04f8 (0x16c bytes each).
    struct FenceBird {
        std::string name;       // +0x00, the descriptor's asset name
        bool animated{};        // +0x40
        bool loop{};            // +0x4c
        bool added{};           // +0x7c
        bool playing{};         // +0x78
        int x{};                // +0x6c
        int y{};                // +0x70
        int layer{};
        double startedAt{};
        int pitch{};            // +0xa8
        int duration{};         // +0xac
        int count{};            // +0xb0, path points 0..count
        std::vector<SDL_Point> path;  // +0xb4
        float t{};              // +0x15c
        float speed{};          // +0x160
        float delay{};          // +0x164
        int state{};            // +0x168
    };
    // The modal list of 0x00420750 (common/dialogue/listdlg) for the tune
    // and instrument choosers. Interim: drawn here until the shared list
    // dialog is ported.
    struct FenceList {
        bool tunes{};
        std::string title;
        std::vector<std::string> items;
        std::vector<fs::path> files;
        int selection{};
    };
    struct FenceGame {
        std::unordered_map<std::string, TexturePtr> bitmaps;
        std::unordered_map<std::string, DecodedMedia> clips;
        std::unordered_map<std::string, TexturePtr> clipTextures;
        // The instrument ini (0x006aa430) as [resource, name key] pairs.
        std::vector<std::pair<std::string, std::string>> instruments;
        int instrument{};                       // 0x006b0444
        std::array<DecodedMedia, 18> sounds;    // 0x006b2c98
        DecodedMedia birdsSound;                // 0x006b3760
        LiveAudioMixer::EffectHandle birdsEffect{};
        double birdsSoundEndsAt{};
        bool entered{};
        FenceMode mode{FenceMode::Free};
        FenceMode previousMode{FenceMode::Free};   // 0x006b3814
        int cursor{};                           // 0x006b1bb8
        int page{};                             // 0x006b3820
        bool letters{};                         // 0x006b381c
        int dragSlot{-1};                       // 0x006b8638
        bool dragging{};
        bool trashShown{};
        int departureSpread{10};                // 0x006b0440
        std::optional<double> playTimerAt;      // 0x006b3818, tag 0
        std::optional<double> markerTimerAt;    // 0x006b0448, tag 1
        double lastUpdate{};                    // 0x006b3810
        // Descriptors: 16 note slots at 0x006b2ce0, four per slot at
        // 0x006b3828 (two ledger lines, the letter, one unused), and the
        // named singletons.
        std::array<FenceSprite, 16> notes;
        std::array<std::array<FenceSprite, 2>, 16> ledgers;
        std::array<FenceSprite, 16> letterLabels;
        FenceSprite cursorSprite{"cursor"};     // 0x005090d8
        FenceSprite marker;                     // 0x00508d90
        FenceSprite trash{"trash"};             // 0x00509180
        FenceSprite modeButton;                 // 0x00508ee0
        FenceSprite lettersBoard{"letters"};    // 0x00508e38
        FenceSprite mike{"mike"};               // 0x00508f88
        FenceSprite player{"player"};           // 0x00509030
        std::vector<FenceBird> birds;           // count 0x006b043c
        // rand() state for the bird paths (MSVC holdrand); seeded by the
        // developer command fence:seed to mirror the capture adapter.
        std::uint32_t randomState{1};
        std::uint32_t randomSeed{1};
        bool randomSeeded{};
        // Panel controls pressed on mouse-down; their 0x116 fires on release.
        std::uint32_t pressedControl{};
        int scrollDragOffset{};
        bool scrollDragging{};
        std::optional<double> waitUntil;
        std::optional<FenceList> list;
        // The name prompt of 0x0041ffb0 for Save (interim, as the list).
        std::optional<std::string> saveName;
        TexturePtr nameBackdrop;
        TexturePtr listBackdrop;
        TexturePtr listButton;
    };

    FenceGame loadFenceGame(Package &package);
    bool beginFenceInteraction(int x, int y, bool rightButton);
    void moveFenceInteraction(int x, int y);
    void endFenceInteraction(int x, int y);
    bool handleFenceKey(SDL_Keycode key);
    bool typeFenceText(const char *utf8);
    void updateFence(double elapsed);
    void renderFence(double elapsed);
    std::optional<std::pair<std::uint32_t, SDL_Rect>>
        fenceHotspotAt(int x, int y) const;
    // Whether the shell's Help and Exit controls answer: 0x00468130 acts
    // on controls only in the free and record states.
    bool fenceControlsEnabled() const;
    // Developer control API for "fence:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeFenceDeveloperCommand(std::string_view command);

    std::optional<FenceGame> fenceGame;
    FenceTune fenceTune;

private:
    void enterFence();
    int fenceRandom();
    int fenceRandomBelow(int bound);
    const TexturePtr &fenceBitmap(const std::string &name);
    const DecodedMedia &fenceClip(const std::string &name);
    void addFenceSprite(FenceSprite &sprite, const std::string &name, int x,
                        int y, int layer, std::uint32_t hotspot = 0U);
    void addFenceSpriteAtDefault(FenceSprite &sprite, const std::string &name,
                                 int x, int y, int layer);
    void removeFenceSprite(FenceSprite &sprite);
    void addFenceBird(FenceBird &bird, int x, int y, int layer, double now);
    void removeFenceBird(FenceBird &bird);
    void selectFenceInstrument(int index);
    void setFenceBpm(int bpm);
    void setFenceModeButton(int control);
    void enterFenceFree();
    void enterFencePlay(double now);
    void enterFenceRecord();
    void leaveFenceMode();
    void stopFencePlay();
    void loadFenceTune(std::optional<int> index, double now);
    bool readFenceTune(const fs::path &path);
    void saveFenceTune(const std::string &name);
    void layoutFenceNotes();
    void placeFenceCursor();
    void scrollFenceToCursor();
    void resetFencePage();
    bool advanceFenceCursor();
    void playFenceNote(int pitch, double now);
    void pressFencePlank(int pitch, double now);
    bool playNextFenceNote(double now);
    void beginFenceNoteDrag(int slot);
    void dragFenceNote(int x, int y);
    void dropFenceNote();
    void cycleFenceNoteValue(int slot, bool dotted);
    void toggleFenceLetters();
    void startFenceDeparture(double now);
    void spawnFenceDepartureBird(int x, int y, const FenceNote &note,
                                 double now);
    void startFenceArrival(double now);
    void spawnFenceArrivalBird(int x, int y, const FenceNote &note,
                               double now);
    bool stepFenceBird(FenceBird &bird, float seconds, double now);
    void updateFenceBirds(float seconds, double now);
    void dispatchFenceHotspot(std::uint32_t hotspot, double now);
    void dispatchFenceControl(std::uint32_t control, double now);
    void openFenceTuneList();
    void openFenceInstrumentList();
    void closeFenceList(bool accept);
    bool pressFenceList(int x, int y);
    void renderFenceList();
    void playFenceBirdsSound();
    void stopFenceBirdsSound();
    int fenceStaveY(int x, int pitch) const;
    int fenceScrollRange() const;
    SDL_Rect fenceSockBounds() const;
    void dragFenceSock(int x);
};

} // namespace fgp
