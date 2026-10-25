#pragma once

// State and services shared by every view of the native game: the engine
// (window, renderer, audio), menu, dialogs, profiles and the story-scene
// shell. Each minigame keeps its own state and logic in a class of its own
// (app_<name>.h) that derives virtually from AppCore, and GameApp (app.h)
// combines them. Functions that dispatch across those classes are declared
// here as pure virtual and implemented by GameApp.

#include <SDL.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

#include "assets/executable_data.h"
#include "assets/fact_table.h"
#include "util/no_repeat.h"
#include "assets/help_table.h"
#include "assets/language_table.h"
#include "assets/package.h"
#include "assets/profile.h"
#include "assets/score_tables.h"
#include "game/constants.h"
#include "game/racing_data.h"
#include "media/audio.h"
#include "util/reference_trace.h"
#include "media/media.h"
#include "media/smacker.h"
#include "render/bitmap_font.h"
#include "render/sdl_util.h"
#include "util/crt_rand.h"
#include "util/reference_trace.h"

namespace fgp {

class AppCore {
public:
    AppCore() = default;
    AppCore(const AppCore &) = delete;
    AppCore &operator=(const AppCore &) = delete;
    virtual ~AppCore() = default;

protected:
    struct MenuClip {
        std::string name;
        int x{};
        int y{};
        bool idle{};
        DecodedMedia media;
        TexturePtr texture;
    };
    struct MenuEffect {
        std::string name;
        int x{};
        int y{};
        int ampLevel{};
        DecodedMedia media;
        TexturePtr texture;
    };
    struct SceneClip {
        std::string name;
        int x{};
        int y{};
        bool loopVideo{};
        DecodedMedia media;
        double duration{};
        TexturePtr texture;
        bool visible{true};
        int layer{};
        std::uint32_t conflictMask{};
    };
    struct SceneCharacterGroup {
        struct StoredTables {
            std::uint32_t baseTableRva{};
            std::uint32_t category2TableRva{};
            std::uint32_t category3TableRva{};
        };
        struct Category4Table {
            std::uint32_t tableRva{};
            std::vector<SceneClip> clips;
        };
        std::uint32_t hotspot{};
        std::uint32_t conflictMask{};
        bool enabled{true};
        std::uint32_t baseTableRva{};
        std::uint32_t category2TableRva{};
        std::uint32_t category3TableRva{};
        std::vector<SceneClip> baseClips;
        std::vector<SceneClip> category2Clips;
        std::vector<SceneClip> category3Clips;
        std::vector<StoredTables> storedTables;
        std::vector<Category4Table> category4Tables;
        std::size_t baseClipIndex{};
        std::size_t category2ClipIndex{};
        std::size_t category4ClipIndex{};
        // Category 3 is the click reaction. 0x0040aa50 marks the group
        // pending when its hotspot is pressed; the reaction replaces the
        // next clip (0x0040a7c0 with category 3) and its index advances
        // through the table in order (0x0040a580).
        std::size_t category3ClipIndex{};
        bool category3Pending{};
        bool category3Playing{};
        // Whether 0x0040ac60 has written the special deadline (+0x58) this
        // visit. Binding (0x0040a2b0) leaves it 0, already expired.
        bool category2DeadlineEverArmed{};
        std::optional<std::size_t> category4TableIndex;
        double baseClipStartedAt{};
        double category2DeadlineAt{};
        bool baseClipActivated{};
        bool baseClipPlaying{};
        bool category2Playing{};
        bool category4Playing{};
        bool category2DeadlineArmed{};
        bool conflictSeen{};
        bool conflictBlocked{};
        // A clip that finished while a scene stage waited for this group.
        // The original's group completion (0x0040abf0 -> 0x0040a7c0) starts
        // nothing then, and nothing removes the finished player: it stays
        // on its last frame until 0x0040a660 removes it to install the
        // stage, or the group resumes. Category-4 clips are released when
        // they finish, so a held one is kept in heldCategory4Clips.
        const SceneClip *heldClip{};
        std::vector<SceneClip> heldCategory4Clips;
    };
    struct SceneAnimationObject {
        std::string name;
        int x{};
        int y{};
        int layer{};
        std::uint32_t hotspot{};
        bool visible{true};
        bool playing{};
        bool hideOnComplete{};
        double startedAt{};
        DecodedMedia media;
        TexturePtr texture;
        // Descriptor +0x4c set: the player repeats until it is removed.
        bool loop{};
        // A one-shot that finished keeps its last frame, as the original's
        // finished player does until the scene removes it.
        bool completed{};
        // Audio carried by the clip, on the effects bus while it plays.
        DecodedMedia audio;
        std::uint64_t audioEffect{};
    };
    // Placed media shared by the Hopscotch, Beemaze, Bugzzz, Buildabike,
    // Hustle, Mountain and Wheelbarrow minigames.
    struct HopscotchMedia {
        DecodedMedia media;
        TexturePtr texture;
    };
    struct SceneBitmapLayer {
        std::string name;
        int x{};
        int y{};
        int layer{};
        int width{};
        int height{};
        TexturePtr texture;
        // False until the scene's callback adds the bitmap player.
        bool visible{true};
    };
    enum class SceneCornerButtonKind { Help, Exit };
    struct SceneCornerButton {
        SceneCornerButtonKind kind{SceneCornerButtonKind::Help};
        SDL_Rect bounds{};
        SurfacePtr normalSurface;
        TexturePtr normalTexture;
        TexturePtr pressedTexture;
    };
    enum class StoryNavigationControl { None, Previous, Next, Help, Exit };
    struct StoryNavigationButton {
        StoryNavigationControl control{StoryNavigationControl::None};
        SDL_Rect relativeBounds{};
        SurfacePtr normalSurface;
        TexturePtr normalTexture;
        TexturePtr pressedTexture;
        TexturePtr disabledTexture;
    };
    struct SceneCollectible {
        std::string name;
        int x{};
        int y{};
        std::uint32_t hotspot{};
        std::int32_t storyIndex{-1};
        bool visible{};
        SurfacePtr surface;
        TexturePtr texture;
    };
    // The parameter record a caller passes as GAME_GotoSceneByName's
    // second argument when it opens the info viewer (buttercol's at
    // 0x00666c48 filled by 0x0043a3b0, racemenu's at 0x006d02f8 filled by
    // 0x0047bcf0); 0x00469290 keeps it at 0x006bb830. Info writes the page
    // it shows back to +0x14 when it ends (0x00469760), where the racing
    // menu reads it on return (0x0047b960).
    struct InfoRequest {
        std::string article;      // +0x04, "<article>.ini"
        std::string returnScene;  // +0x08
        int returnMode{};         // +0x0c
        int awardCategory{-1};    // +0x10, negative: every page is open
        int page{};               // +0x14, in and out (0x00469760)
    };

    // A view of the scene loadScene is constructing, before it replaces the
    // active one. Story scenes build their contents in their own files
    // (<Name>Scene::build<Name>Scene) through this view: every member refers
    // to loadScene's in-progress state or one of its helpers, so it is valid
    // only during that loadScene call.
    struct SceneBuild {
        const std::string &sceneName;
        int entryMode;
        std::uint32_t sceneVisitCount;
        std::unique_ptr<Package> &nextPackage;
        std::unique_ptr<Package> &nextEditionPackage;
        std::unique_ptr<Package> &nextData2Package;
        std::unique_ptr<Package> &nextLanguagePackage;
        std::unique_ptr<Package> &nextAddonPackage;
        std::vector<SceneClip> &nextSceneClips;
        std::vector<SceneCharacterGroup> &nextSceneCharacterGroups;
        std::vector<SceneBitmapLayer> &nextSceneBitmapLayers;
        std::vector<SceneCollectible> &nextSceneCollectibles;
        bool &nextSceneUsesAudioTimeline;
        bool &nextSceneSequenceBlocksInput;
        std::optional<std::pair<double, std::string>> &nextSceneTimedTransition;
        TexturePtr &nextSceneTitleTexture;
        int &nextSceneTitleWidth;
        int &nextSceneTitleHeight;
        std::function<std::span<const std::uint8_t>(std::string)>
            sceneAnimationData;
        std::function<double(std::string_view, double)> appendSceneNarration;
        std::function<double(const ExecutableGameData::AnimationDefinition &,
                             double)>
            appendSceneDefinition;
        std::function<void(std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t, std::uint32_t)>
            addSceneCharacterGroupWithTables;
        std::function<void(std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t)>
            addSceneCharacterStoredTables;
        std::function<void(std::uint32_t, std::uint32_t)>
            addSceneCharacterCategory4Table;
        std::function<void(Package &, const char *, int, int, std::uint32_t,
                           std::int32_t, bool)>
            addSceneCollectible;
        std::function<void(std::uint32_t, int, bool)> addSceneAnimationObject;
        // A descriptor player that runs beside the sequence: a looping one
        // starts with the scene, a one-shot waits for playSceneAnimationObject.
        std::function<void(std::uint32_t, bool)> addScenePlayer;
        std::function<void(Package &, std::string_view, int, int)>
            addMenuBitmap;

        // Keeps the default arguments of loadScene's helper.
        void addSceneCharacterGroup(std::uint32_t hotspot,
                                    std::uint32_t conflictMask,
                                    std::uint32_t tableRva,
                                    std::uint32_t category2TableRva = 0U,
                                    std::uint32_t category3TableRva = 0U) {
            addSceneCharacterGroupWithTables(hotspot, conflictMask, tableRva,
                                             category2TableRva,
                                             category3TableRva);
        }
    };
    enum class CursorKind {
        Default,
        Wait,
        Character,
        Factopedia,
        Activity,
        Subgame,
        Sound,
        Music,
        Cycle,
        RaceStart,
        HandUp,
        HandDown,
        HandLeft,
        HandRight,
        StoryPage,
        ActionGame,
        Count,
    };
    struct CursorAnimation {
        const char *name{};
        DecodedMedia media;
        TexturePtr texture;
    };

    static constexpr std::array<const char *, static_cast<std::size_t>(CursorKind::Count)>
        cursorNames{
            "default", "wait", "character", "factopedia", "activity",
            "subgame", "sound", "music", "cycle", "race_start",
            "hand_up", "hand_down", "hand_left", "hand_right",
            "storypage", "actngame",
        };

    enum class ViewMode { Menu, Options, Scene };
    struct NavigationTarget {
        ViewMode mode{ViewMode::Menu};
        std::string scene;
        std::string label;
        int entryMode{0};
    };
    enum class DialogMode {
        None,
        Name,
        Calendar,
        Message,
        Award,
        HighScore,
        Help,
    };
    enum class HighScoreTab { Profile, Global, Union };
    struct HighScoreRow {
        std::string player;
        std::string date;
        std::uint32_t score{};
    };
    enum class OptionsControl {
        None,
        EffectDown,
        EffectTrack,
        EffectUp,
        SpeechDown,
        SpeechTrack,
        SpeechUp,
        MusicDown,
        MusicTrack,
        MusicUp,
        SubtitlePrevious,
        SubtitleNext,
        SubtitleToggle,
        Ok,
        Cancel,
    };
    enum class DialogControl {
        None,
        NameOk,
        NameCancel,
        CalendarPrevious,
        CalendarNext,
        CalendarOk,
        CalendarDay,
        MessagePrimary,
        MessageSecondary,
        AwardClose,
        HighScoreProfile,
        HighScoreGlobal,
        HighScoreUnion,
        HighScoreClose,
        HelpOk,
        HelpPrevious,
        HelpNext,
        HelpScrollUp,
        HelpScrollDown,
        HelpScrollTrack,
        HelpTopic,
    };
    enum class MessageKind {
        Notice,
        ExitConfirmation,
        Fact,
        MorningRideConfirmation,
        BuildacarDemolishConfirmation,
        WhackamoleNextLevel,
        WhackamoleGameOver,
        BalloonhuntGameOver,
        ButterflyGameOver,
        AudiopairsGameOver,
        SockdrawerGameOver,
        BuildabikeGameOver,
        BeemazeGameOver,
        HustleGameOver,
        HopscotchGameOver,
        TriviaGameOver,
        LettersortGameOver,
        ChessAbortConfirmation,
        WheelbarrowGameOver,
        TextinvaderGameOver,
        PuzzleGameOver,
        PipelineLevelComplete,
        PipelineGameOver,
        MathlabGameOver,
        DraughtsGameOver,
        DraughtsAbortConfirmation,
        ActivitypageClearConfirmation,
        ActivitypagePrintConfirmation,
        TournamentPlayerReady,
        TournamentAbortConfirmation,
        ColorfillConfirmation,
        // race:CONFIRM, the race's Escape (0x0047a7f0).
        RacingQuitConfirmation,
        // race:CRASHED, a human car wrecked (0x004776e0 step 10).
        RacingCrashed,
    };

    // 0x0040a150: ordered first cycle, then random no-repeat cycles.
    std::size_t nextNoRepeatOption(NoRepeatState &state,
                                   std::size_t optionCount) {
        return fgp::nextNoRepeatOption(
            state, optionCount, [this](int bound) {
                return particleRandom(bound);
            });
    }

    void uploadVideoFrame(SDL_Texture *texture, const VideoFrame &frame);
    void invalidateUploadedVideoFrames();

    void setMenuTitle(std::string_view message = {});
    void resetAnimationClock();
    // Implemented by GameApp, which also resets every minigame on the way.
    virtual void enterMenu(bool restartMusic = true) = 0;
    void enterOptions();
    void leaveOptions(bool accept);
    // 0x0040d860 -> 0x0040b490: reads the selected profile's options (the
    // defaults without one) and applies the three volumes at once. The
    // original does this at startup and from the menu entries that select
    // the profile (Settings, the scenes behind 4-8 and 13), not when the
    // profile arrows change the selection.
    void loadProfileOptions(bool applyToMusic = true);
    void saveProfileOptions();
    // sounddlg:COUNTRIES: "CODE:Name" entries (0x00422140).
    std::vector<std::string> optionsCountries() const;
    // 0x004227b0: the first entry starting with `code` up to any ';'
    // (_strnicmp), or `fallback`.
    int optionsCountryIndex(std::string_view code, int fallback) const;
    // The slider value 0x004230ae stores: the integer times the float 0.01.
    static float optionsSliderValue(int value);
    void enterScene(const std::string &sceneName, std::string_view label,
                    int entryMode = 0);
    void returnFromScene();
    // 0x0040cc30, the shared Escape route's end: the registry parent.
    void leaveToRegistryParent();
    void selectSceneIndexTab(std::size_t tabIndex);
    std::optional<std::size_t> sceneIndexTabAt(int x, int y) const;
    void showSceneHelp();
    bool openStoryNavigation(int x, int y);
    void closeStoryNavigation();
    StoryNavigationControl storyNavigationControlAt(int x, int y) const;
    bool activateStoryNavigation(StoryNavigationControl control);
    void showMenuHelp();
    void openHelpPages(std::vector<HelpPage> pages);
    void selectHelpTopic(std::size_t index);
    void scrollHelpBody(int lines);
    std::span<const std::uint8_t> activeSceneAnimationData(std::string descriptorName);
    // How scene_DrawAnim keys the clip of the descriptor named `name`: on its
    // top-left pixel when the descriptor's +0x48 is set. A descriptor the
    // scene's own code references wins; otherwise every visible descriptor
    // of that name must agree. Clips without one keep the native policy.
    SmackerKeying sceneClipKeying(std::string_view scene,
                                  std::string_view name) const;
    SmackerKeying activeSceneClipKeying(std::string_view name) const {
        return sceneClipKeying(activeScene, name);
    }
    // The active scene's clip `name`, keyed as its descriptor says; missing
    // audio is allowed, as most minigame clips are silent.
    DecodedMedia decodeActiveSceneClip(std::string name,
                                       bool decodeVideo = true,
                                       bool decodeAudio = true);
    void startSceneSequence(const std::vector<ExecutableGameData::AnimationDefinition> &definitions, std::string_view label);
    void seedOpeningCharacterGroups();
    void skipSceneForeground();
    void playSceneSound(std::uint32_t descriptorRva, std::string_view label);
    void startSceneAnimation(std::uint32_t descriptorRva, std::string_view label);
    void playStandaloneNarration(std::string_view asset,
                                 std::string_view label);
    void showAward(std::string_view iconAsset, std::string_view titleKey,
                   std::string_view descriptionKey, bool traceIcon = true);
    // The same dialog with an already resolved title and description.
    void showAwardText(std::string_view iconAsset, std::string title,
                       std::string description, bool traceIcon = true);
    void showMedalAward(std::uint32_t partNumber, std::uint32_t medalLevel);
    void showCarPartAward(std::uint32_t partNumber);
    void showFactCompletionAward(std::uint32_t partNumber);
    void showAnaglyphAward();
    void hideSceneCollectible(std::string_view name);
    // `listKey` is the score table's profile record key (0x0041f8c0's
    // second argument); Mountain passes its track index. `flags` is
    // 0x0041ee50's fourth argument (assets/score_tables.h): 0 for points.
    void showHighScore(int score, std::string_view titleKey = "GAMENAME",
                       std::uint32_t listKey = 0U, std::uint32_t flags = 0U);
    // 0x0041f030 (and 0x0041f640 for the active scene): the same dialog
    // showing a scene's three lists without submitting a score, as the
    // racing menu's best-times buttons and the puzzles' High score do.
    // `currentRow` is the boxed row (0x0041f030's fourth argument): the
    // puzzles pass the last row they submitted, when there is one.
    void showScoreTable(std::string_view scene, std::string caption,
                        std::uint32_t listKey, std::uint32_t flags,
                        const std::optional<ScoreRow> &currentRow = std::nullopt);
    // 0x0041ee50 with no dialog, as Rubberball (0x0047dcbe), Jigsaw
    // (0x0046ad81), Sliding (0x0046ee83) and Mahjong (0x00450c83) call it:
    // the active scene's record is written and the submitted row returned.
    ScoreRow submitSceneScore(std::uint32_t score, std::uint32_t listKey,
                              std::uint32_t flags);
    // The profile list the score tables read (0x00411d40).
    ScoreProfiles scoreProfiles() const;

    // The tournament (0x00588fe0-0x0058905f and the score lists at
    // 0x006ea760); the scenes are in app_tournament.cpp.
    struct TournamentPlayer {
        std::string name;   // slot +4
        int difficulty{1};  // slot +0x24: 0 easy, 1 medium, 2 hard
    };
    struct TournamentGame {
        std::string scene;       // record +0
        std::uint32_t version{}; // record +0x24, the entry mode
        std::string title;       // record +0x20
    };
    // A row of a game's list: the score, and the placement points the
    // results screen gives it (0x0041aaf0).
    struct TournamentRow {
        std::string player;
        std::uint32_t points{};
        std::uint32_t score{};
    };
    struct TournamentState {
        bool active{};                 // 0x0058905c
        std::string profile;           // 0x00588fe0
        std::vector<TournamentPlayer> players; // 0x00588ffc, at most 8
        std::vector<TournamentGame> games;     // 0x00589024, at most 12
        int player{};                  // 0x00589020
        int game{};                    // 0x00589058
        std::vector<std::vector<TournamentRow>> scores; // 0x006ea760
    };
    TournamentState tournament;
    // 0x0041f8c0's tournament branch: 0x004196c0 then 0x00419740.
    void recordTournamentScore(int score);
    // 0x0040cd90 -> 0x00419410: leaving a round asks first.
    bool askTournamentAbort();
    // 0x00419450: clear the flag and return to the setup screen.
    void abortTournament();
    // 0x00419790: the results screen's Neste/Avslutt.
    void advanceTournamentGame();
    // 0x00419660: the PLAYERREADY box every round opens with.
    void showTournamentPlayerReady();
    // A subgame.ini game's display name: its name%d language key, or the
    // scene table's title key (src/generated/game_manifest.json).
    std::string subgameTitle(const std::string &scene,
                             const std::string &nameKey) const;
    // After PLAYERREADY: the game's own tournament start (GameApp
    // dispatches it to the active minigame).
    virtual void startTournamentRound(int difficulty) { (void)difficulty; }
    void persistHighScore();
    void fillHighScoreRows(const ScoreLists &lists);
    void showBonusCarPartAward(std::uint32_t partNumber);
    void fillSharedGlobalRows();
    void openHighScoreDialog();
    void showMessage(std::string message, MessageKind kind = MessageKind::Notice);
    void showMessageWithTitle(std::string messageTitle, std::string message,
                              MessageKind kind = MessageKind::Notice);
    void showExitConfirmation();
    void showMorningRideConfirmation();
    // The Yard's mailbox_1 bitmap player (app_yard.cpp).
    void setYardMailboxVisible(bool visible);
    // The story ambience 0x0040b0f0 starts with Intro, Yard, House, Morning,
    // Outtent and Hopscotch: one of ambience1..4 picked by rand() % 4
    // (0x0040b110), and a fresh pick each time one finishes (0x0040b1b0).
    void startSceneAmbience(double now);
    void updateSceneAmbience(double now);
    void stopSceneAmbience();
    std::array<DecodedMedia, 4> sceneAmbienceTracks;
    bool sceneAmbienceDecoded{};
    std::optional<std::size_t> sceneAmbienceIndex;
    double sceneAmbienceEndsAt{};
    // Help and the RIDEDOWN box hold the game clock while they are open.
    [[nodiscard]] bool dialogClockFrozen() const {
        return dialogMode == DialogMode::Help ||
               (dialogMode == DialogMode::Message &&
                (dialogMessageKind == MessageKind::MorningRideConfirmation ||
                 dialogMessageKind == MessageKind::RacingQuitConfirmation ||
                 dialogMessageKind == MessageKind::RacingCrashed));
    }
    void openNameDialog();
    void acceptNameDialog();
    void acceptCalendarDialog();
    double animationTimeNow();
    bool playSceneAnimationObject(std::string_view name);
    void setSceneCharacterGroupEnabled(std::uint32_t hotspot, bool enabled,
                                       bool seededDeadlineExpired = false);
    std::vector<SceneClip> decodeSceneCharacterTable(
        std::uint32_t tableRva, std::uint32_t conflictMask);
    void clearSceneCharacterCategory4(SceneCharacterGroup &group);
    void clearSceneCharacterHold(SceneCharacterGroup &group);
    bool requestSceneCharacterReaction(std::uint32_t hotspot);
    void reseedSceneCharacterGroups(double timelineTime);
    bool sceneCharacterReactionBlocksInput() const;
    void replaceSceneCharacterGroupTables(
        std::uint32_t conflictMask, std::uint32_t baseTableRva,
        std::uint32_t category2TableRva, std::uint32_t category3TableRva,
        double timelineTime);
    void startSceneCharacterCategory4(std::uint32_t conflictMask,
                                      std::uint32_t tableRva,
                                      double timelineTime);
    const SceneClip *activeSceneCharacterClip(
        const SceneCharacterGroup &group) const;
    std::vector<double> scheduleSceneClipStarts(
        const std::vector<SceneClip> &clips, double requestedAt,
        std::uint32_t initialConflictMask) const;
    void armSceneCharacterDeadline(SceneCharacterGroup &group,
                                   double timelineTime);
    void acquireSceneCharacterConflicts(std::uint32_t conflictMask,
                                        double timelineTime);
    double sceneCharacterConflictReleaseAt(
        std::uint32_t conflictMask, double timelineTime) const;
    void updateSceneCharacterGroups(double timelineTime);
    bool showFactPage(std::uint32_t hotspot);
    // Follows a hotspot recovered from the original's scene dispatchers into
    // the scene or minigame it leads to. Returns false when nothing in the
    // recovered table covers this hotspot.
    bool navigateGeneratedHotspot(std::uint32_t hotspot);
    // Narration scheduling, mirroring the original's one-shot timers rather
    // than pre-mixing every clip onto a fixed timeline at scene-build time.
    void armSceneNarration(std::string_view name, double delaySeconds,
                           bool replacesSequenceInOriginal,
                           std::optional<double> armedAt = std::nullopt);
    void startSceneNarration(std::string_view name,
                             bool replacesSequenceInOriginal,
                             std::optional<DecodedMedia> predecoded =
                                 std::nullopt,
                             std::optional<double> durationOverride =
                                 std::nullopt);
    void updatePendingNarrations();
    // 0x004283e0 (yard) and 0x00424c60 (desk), called from the top of their
    // mouse-down and key-press handlers.
    void cancelPendingNarrations();
    // Ends narration entirely -- pending and playing -- which is what
    // leaving a scene does (EVENT_EndScene 0x104 clears the timer slots).
    void stopSceneNarration(std::string_view reason = "scene_ended");
    // The desk's key-1 counter (record 0x402, key 1) gates S02AN-NAR-002:
    // 0x00424dcb plays it only while the counter is still zero, and
    // 0x00424de4 persists the increment afterwards.
    [[nodiscard]] bool deskNarrationPending();
    void consumeDeskNarrationCredit();
    int particleRandom(int bound);
    int particleRandomValue();
    void startAmpEffect(int level);
    SDL_Point optionsDialogPosition();
    OptionsControl optionsControlAt(int x, int y);
    static int optionsChannelForControl(OptionsControl control);
    // Returns whether the slider's value changed.
    bool setOptionsSliderFromMouse(int channel, int x);
    void setOptionsVolume(int channel, float volume);
    void previewOptionsChannel(int channel);
    void stopOptionsPreview(int channel);
    // A scene's [Volume] section in sound.ini (0x0040b630): the per-scene
    // factor for the effect, speech and music channels, 1.0 when absent.
    static std::array<float, 3> readSceneAudioGains(const Package &package);
    // Writes FGP.exe's set-2 music factor (0x004a68dc) through 0x0040b410,
    // which stores it and applies it to the running music at once
    // (0x0040b430). Only story scene callbacks write it: Intent and the TV
    // room duck the music to 0.25 under certain clips, Outtent silences it
    // under its dream clips, and each restores 1.0 later. Nothing resets it
    // on a scene change, so it carries into the next scene until a restore.
    void setMusicDuck(float duck);
    // The music restore a scene's EndScene handler makes as it is left.
    void leaveSceneMusicDuck(std::string_view leftScene);
    // A clip player's volume for its audio: the effect channel for Smacker
    // track 0, the speech channel otherwise (0x00403068, 0x00403070).
    [[nodiscard]] float clipAudioVolume(const DecodedMedia &media) const;
    std::vector<std::string> messageLines();
    int messageTextHeight();
    int messageDialogHeight();
    std::vector<std::string> helpBodyLines() const;
    int helpMaximumBodyScroll() const;
    SDL_Rect helpScrollThumbBounds() const;
    std::optional<std::size_t> helpTopicAt(int x, int y) const;
    int calendarDayAt(int x, int y);
    CursorKind cursorKindForName(std::string_view name);
    double sceneElapsedNow();
    std::pair<const SceneClip *, std::size_t> sceneCharacterPoseAt(
        const SceneCharacterGroup &group, double timelineTime);
    bool sceneCharacterVisible(const SceneCharacterGroup &group);
    std::optional<std::pair<std::uint32_t, SDL_Rect>> sceneAnimationObjectAt(int x, int y);
    std::optional<std::pair<std::uint32_t, SDL_Rect>> sceneCollectibleAt(int x, int y);
    std::optional<std::size_t> sceneCornerButtonAt(int x, int y);
    std::optional<std::size_t> menuCornerButtonAt(int x, int y);
    // Implemented by GameApp, which also consults the minigame hit tests.
    virtual std::uint32_t hotspotAt(int x, int y) = 0;
    virtual std::optional<SDL_Rect> currentHotspotBounds(std::uint32_t hotspot) = 0;
    CursorKind cursorForContext();
    // The selected profile's file, or nullopt when none is selected or it
    // does not exist.
    std::optional<fs::path> activeProfilePath() const {
        if (profileIndex >= profiles.size() ||
            profiles[profileIndex] == "No profile")
            return std::nullopt;
        fs::path path = profileDirectory / (profiles[profileIndex] + ".pfl");
        std::error_code error;
        if (!fs::exists(path, error)) return std::nullopt;
        return path;
    }

    fs::path gameDirectory;
    Package menu;
    Package common;
    ReferenceTrace referenceTrace;
    ExecutableGameData executableGameData;
    std::unique_ptr<Package> editionCommon;
    std::unique_ptr<Package> data2Common;
    std::unique_ptr<Package> addonCommon;
    std::unique_ptr<LanguageTable> storyIndexConfiguration;
    LanguageTable language;
    DecodedMedia animation;
    DecodedMedia music;
    std::vector<MenuClip> menuClips;
    std::vector<MenuEffect> menuEffects;
    std::array<CursorAnimation, static_cast<std::size_t>(CursorKind::Count)> cursors;
    SDL_Window *window{nullptr};
    SDL_Renderer *renderer{nullptr};
    // Frame metadata storage is stable for a loaded clip even when its bounded
    // BGRA window evicts and reallocates pixels. Remember the frame object,
    // rather than its allocation address (which an allocator may reuse), so
    // 10-15 fps Smacker frames are not redundantly uploaded at 60 Hz.
    std::unordered_map<SDL_Texture *,
                       std::pair<const VideoFrame *, std::uint64_t>>
        uploadedVideoFrames;
    SurfacePtr backdrop;
    SurfacePtr hotspots;
    SurfacePtr title;
    TexturePtr backdropTexture;
    TexturePtr hotspotTexture;
    TexturePtr titleTexture;
    std::array<SceneCornerButton, 2> menuCornerButtons;
    std::optional<std::size_t> pressedMenuCornerButton;
    TexturePtr modalMeshTexture;
    bool hasSubtitleSettings{false};
    SurfacePtr settingsBackdrop;
    SurfacePtr settingsButtonNormal;
    SurfacePtr settingsButtonPressed;
    SurfacePtr settingsDownNormal;
    SurfacePtr settingsDownPressed;
    SurfacePtr settingsUpNormal;
    SurfacePtr settingsUpPressed;
    SurfacePtr settingsThumb;
    SurfacePtr settingsToggleNormal;
    SurfacePtr settingsTogglePressed;
    TexturePtr settingsBackdropTexture;
    TexturePtr settingsButtonNormalTexture;
    TexturePtr settingsButtonPressedTexture;
    TexturePtr settingsDownNormalTexture;
    TexturePtr settingsDownPressedTexture;
    TexturePtr settingsUpNormalTexture;
    TexturePtr settingsUpPressedTexture;
    TexturePtr settingsThumbTexture;
    TexturePtr settingsToggleNormalTexture;
    TexturePtr settingsTogglePressedTexture;
    SurfacePtr nameDialogBackdrop;
    SurfacePtr nameDialogHotspots;
    SurfacePtr nameDialogOk;
    SurfacePtr nameDialogCancel;
    TexturePtr nameDialogBackdropTexture;
    TexturePtr nameDialogOkTexture;
    TexturePtr nameDialogCancelTexture;
    SurfacePtr calendarBackdrop;
    SurfacePtr calendarHotspots;
    SurfacePtr calendarMark;
    SurfacePtr calendarPrevious;
    SurfacePtr calendarNext;
    SurfacePtr calendarOk;
    TexturePtr calendarBackdropTexture;
    TexturePtr calendarMarkTexture;
    TexturePtr calendarPreviousTexture;
    TexturePtr calendarNextTexture;
    TexturePtr calendarOkTexture;
    SurfacePtr messageBackdrop;
    SurfacePtr messageButtonNormal;
    SurfacePtr messageButtonPressed;
    TexturePtr messageBackdropTexture;
    TexturePtr messageButtonNormalTexture;
    TexturePtr messageButtonPressedTexture;
    SurfacePtr awardBackdrop;
    SurfacePtr awardButtonNormal;
    SurfacePtr awardButtonPressed;
    TexturePtr awardBackdropTexture;
    TexturePtr awardButtonNormalTexture;
    TexturePtr awardButtonPressedTexture;
    SurfacePtr highScoreBackdrop;
    std::array<SurfacePtr, 3> highScoreSelectedTabs;
    SurfacePtr highScoreClosePressed;
    TexturePtr highScoreBackdropTexture;
    std::array<TexturePtr, 3> highScoreSelectedTabTextures;
    TexturePtr highScoreClosePressedTexture;
    SurfacePtr storyNavigationBackdrop;
    TexturePtr storyNavigationBackdropTexture;
    std::array<StoryNavigationButton, 4> storyNavigationButtons;
    SurfacePtr helpBackdrop;
    SurfacePtr helpButtonNormal;
    SurfacePtr helpButtonPressed;
    SurfacePtr helpPreviousNormal;
    SurfacePtr helpPreviousPressed;
    SurfacePtr helpNextNormal;
    SurfacePtr helpNextPressed;
    SurfacePtr helpUpNormal;
    SurfacePtr helpUpPressed;
    SurfacePtr helpDownNormal;
    SurfacePtr helpDownPressed;
    SurfacePtr helpThumb;
    TexturePtr helpBackdropTexture;
    TexturePtr helpButtonNormalTexture;
    TexturePtr helpButtonPressedTexture;
    TexturePtr helpPreviousNormalTexture;
    TexturePtr helpPreviousPressedTexture;
    TexturePtr helpNextNormalTexture;
    TexturePtr helpNextPressedTexture;
    TexturePtr helpUpNormalTexture;
    TexturePtr helpUpPressedTexture;
    TexturePtr helpDownNormalTexture;
    TexturePtr helpDownPressedTexture;
    TexturePtr helpThumbTexture;
    BitmapFont dialogFont;
    BitmapFont calendarFont;
    BitmapFont menuTitleFont;
    BitmapFont messageFont;
    TexturePtr animationTexture;
    int animationWidth{0};
    int animationHeight{0};
    LiveAudioMixer audioMixer;
    // FGP_AUDIO_DUMP: the mixer writes what it renders to that WAV file.
    bool audioDumpActive{false};
    SDL_AudioDeviceID audioDevice{0};
    std::array<float, 3> audioVolumes{};
    bool musicEnabled{true};
    bool running{true};
    bool paused{false};
    bool showHotspots{false};
    std::size_t currentFrame{0};
    double pausedAt{0.0};
    std::chrono::steady_clock::time_point start;
    ViewMode viewMode{ViewMode::Menu};
    // A minigame asks for the wait cursor (Chess while the computer moves).
    bool minigameBusyCursor{};
    DialogMode dialogMode{DialogMode::None};
    HighScoreTab highScoreTab{HighScoreTab::Profile};
    int highScoreValue{};
    std::uint32_t highScoreListKey{};
    std::uint32_t highScoreTimestamp{};
    std::uint32_t highScoreMedalLevel{};
    bool pendingHighScoreAfterAward{false};
    std::uint32_t highScoreFlags{};
    // 0x0041f0e0 boxes the submitted row; the viewer 0x0041f030 has none.
    bool highScoreHasCurrentRow{true};
    // Car parts for raising the lowest medal (0x0041ef6c), shown after the
    // medal award and before the score dialog.
    std::uint32_t pendingBonusCarParts{};
    std::string highScoreGameName;
    std::string highScorePlayer;  // the submitted row's name (0x00411eb0)
    std::string highScoreDate;
    std::vector<HighScoreRow> highScoreProfileRows;
    std::vector<HighScoreRow> highScoreGlobalRows;
    std::vector<HighScoreRow> highScoreUnionRows;
    OptionsControl optionsPressedControl{OptionsControl::None};
    int optionsDraggingChannel{-1};
    // The settings dialog's preview players for the effect and speech
    // sliders, `../../common/sound/effect` and `speech` (0x00422a00).
    std::array<DecodedMedia, 2> optionsPreviewMedia;
    std::array<bool, 2> optionsPreviewLoaded{};
    std::array<LiveAudioMixer::EffectHandle, 2> optionsPreviewHandles{};
    std::array<float, 3> savedAudioVolumes{};
    // The dialog's working copies: an index into sounddlg:COUNTRIES and the
    // on/off toggle ([0x005b3b64], [0x005b3b60]).
    int subtitleLanguage{2};
    int savedSubtitleLanguage{2};
    bool subtitlesEnabled{false};
    bool savedSubtitlesEnabled{false};
    // What the original keeps between dialogs and saves in the profile's
    // `options` record: the subtitle flag 0x0053dac4, its language code
    // 0x0053a1b8 and the second code 0x006ea158 the dialog passes through.
    ProfileOptions profileOptionValues;
    std::string activeScene;
    std::unique_ptr<Package> scenePackage;
    std::unique_ptr<Package> sceneEditionPackage;
    std::unique_ptr<Package> sceneData2Package;
    std::unique_ptr<Package> sceneAddonPackage;
    std::unique_ptr<Package> sceneLanguagePackage;
    bool sceneIsMinigameShell{false};
    SurfacePtr sceneBackdrop;
    SurfacePtr sceneHotspots;
    TexturePtr sceneBackdropTexture;
    TexturePtr sceneHotspotTexture;
    TexturePtr sceneTitleTexture;
    int sceneTitleWidth{0};
    int sceneTitleHeight{0};
    std::vector<TexturePtr> sceneIndexTiles;
    std::vector<SDL_Rect> sceneIndexTileBounds;
    std::vector<TexturePtr> sceneIndexMedals;
    std::vector<SDL_Rect> sceneIndexMedalBounds;
    std::vector<std::string> sceneIndexNames;
    std::vector<bool> sceneIndexUnlocked;
    std::vector<CursorKind> sceneIndexCursorKinds;
    std::array<TexturePtr, 2> sceneIndexTabTextures;
    TexturePtr sceneIndexGoldFrameTexture;
    std::size_t sceneIndexTab{0};
    std::vector<SceneClip> sceneClips;
    std::vector<double> sceneClipStartOffsets;
    std::vector<SceneCharacterGroup> sceneCharacterGroups;
    std::vector<SceneAnimationObject> sceneAnimationObjects;
    std::vector<SceneBitmapLayer> sceneBitmapLayers;
    std::vector<SceneCornerButton> sceneCornerButtons;
    // The parameter block a scene passes when it enters the activity page
    // (0x0045c050 copies scene name, entry mode and picture; Color Fill's
    // "activity" control at 0x00462220 sends its canvas this way).
    struct ActivityEntryParameter {
        std::string scene;
        int mode{};
        SurfacePtr picture;
    };
    std::optional<ActivityEntryParameter> pendingActivityEntry;
    std::optional<std::size_t> pressedSceneCornerButton;
    std::vector<NavigationTarget> navigationHistory;
    bool storyNavigationOpen{false};
    // Whether the active scene opens the popup at all: the scenes that call
    // 0x00421560, Birthday included with both arrows disabled.
    bool storyNavigationAvailable{false};
    SDL_Point storyNavigationOrigin{};
    StoryNavigationControl pressedStoryNavigationControl{
        StoryNavigationControl::None};
    std::string storyPreviousScene;
    std::string storyNextScene;
    int storyPreviousMode{0};
    int storyNextMode{0};
    std::vector<SceneCollectible> sceneCollectibles;
    std::optional<InfoRequest> infoRequest;
    // The entry block activity copies at 0x0045c08a/0x0045c093: +0 return
    // scene, +8 picture. Bouquet's print control fills it at 0x0045ec49.
    std::string activityReturnScene;
    SurfacePtr activityPicture;
    FactTable sceneFacts;
    std::uint32_t sceneFactsViewed{0};
    DecodedMedia sceneAudioTimeline;
    DecodedMedia sceneEffectTimeline;
    DecodedMedia sceneBackgroundMusic;
    std::array<float, 3> sceneAudioGains{1.0F, 1.0F, 1.0F};
    std::string sceneBackgroundMusicAsset;
    bool sceneUsesAudioTimeline{false};
    bool sceneSequenceBlocksInput{false};
    bool sceneSequenceFinished{false};
    // The finished last stage still holds a conflict bit, so its last frame
    // stays on screen (0x0040aad7) until a group with that bit takes over.
    bool sceneFinalStageHeld{false};
    // A finished stage that holds a conflict bit and stays on its last frame
    // while the next stage waits for the other groups it needs (0x0040aad7
    // removes a finished stage only when its mask is 0).
    std::optional<std::size_t> sceneHeldStageIndex;
    // Story clips drawn on the last rendered frame, traced on change.
    std::string sceneDrawnClips;
    void releaseSceneFinalStage();
    void releaseSceneHeldStage();
    std::size_t sceneClipIndex{0};
    double sceneClipTimelineStart{0.0};
    double sceneSequenceRequestedAt{0.0};
    double sceneAudioTimelineStartedAt{0.0};
    bool sceneClipStarted{false};
    std::uint32_t sceneSequenceHeldConflictMask{0};
    std::uint32_t sceneSequencePendingConflictMask{0};

    // Narration the scene has armed but not yet played: the original's timer
    // slots at 0x0054b920, added with 0x0040c480(deadline, interval, tag) and
    // an interval of 0 for one-shot. Both story scenes cancel their pending
    // narration on the first click or key press (yard 0x004283e0, desk
    // 0x00424c60), so this cannot be a fixed timeline.
    struct PendingNarration {
        std::string name;
        double dueSeconds{};
        // Native Smacker decoding is much more expensive than the original's
        // descriptor dispatch. Decode when the timer is armed so that work
        // does not become artificial delay after the recovered deadline.
        DecodedMedia clip;
        // Which primitive the original starts this clip with. 0x0040a770
        // installs it as a one-stage sequence and therefore replaces the
        // running one; 0x00412b90 plays it alongside. We always mix on the
        // independent narration layer, so this records provenance and does not
        // yet drive behaviour -- see startSceneNarration.
        bool replacesSequenceInOriginal{};
    };
    std::vector<PendingNarration> pendingNarrations;
    // The visit count recorded on entry (profile record 0x400 key 0), kept so
    // triggers that fire later -- the desk's narration arms on sequence
    // completion -- can apply the same first-visit gate the original does.
    std::uint32_t activeSceneVisitCount{0};
    DecodedMedia sceneNarrationClip;
    std::string sceneNarrationName;
    double sceneNarrationStartedAt{0.0};
    // Length of the playing clip, so its completion can be reported to the
    // reference trace the same way a visual clip's is.
    double sceneNarrationDuration{0.0};
    // TV room starts the invisible/audio-only SS-003c concurrently when
    // SS-002a finishes. It is decoded during scene load so media completion,
    // not native decode work, controls the follow-up sequence.
    std::optional<DecodedMedia> tvroomConcurrentClip;
    bool tvroomConcurrentFollowupPending{false};
    // Intent's SS-002b start handler (0x00425b20) arms timer 0 for exactly
    // 43,000 ms; expiry restores the music, the baseline group tables and
    // starts the three T03 bridges.
    std::optional<double> intentGroupRestoreAt;
    // When the navigate overlay suspended the narration countdown, so the
    // remaining time can be preserved across it. 0 means not suspended.
    double narrationSuspendedAt{0.0};
    std::mt19937 randomEngine;
    // The C runtime rand() state the original's scenes share (0x0048d323),
    // seeded from the time at startup as 0x0040bfc4 does.
    CrtRandom crtRandom;
    // The race's parameter block, as the racing menu's Start leaves it at
    // 0x006cfed0 (0x0047bfa0); empty for the story race.
    std::optional<racing::RaceBlock> raceParameterBlock;
    // 0x006d05c8-0x006d0633: the last menu race's submitted times, which the
    // racing menu's best-times tables box (0x0047c32f, 0x0047c39b) while the
    // track (and, for race times, the lap count) still match. Start clears
    // the flag and stores the track and laps (0x0047bfdf-0x0047bfee); the
    // results set it (0x0047aa00(1) at 0x0047754c).
    struct RaceScoreRows {
        bool recorded{};   // 0x006d05c8
        int track{};       // 0x006d05cc
        int laps{};        // 0x006d05d0
        ScoreRow raceTime; // 0x006d05d4
        ScoreRow lapTime;  // 0x006d05f8
    } raceScoreRows;
    struct GeneratedClickBag {
        std::vector<std::uint32_t> sourceChoices;
        NoRepeatState selector;
    };
    std::unordered_map<std::string, GeneratedClickBag> generatedClickBags;
    int activeAmpLevel{-1};
    double ampEffectStart{0.0};
    bool fuseActive{false};
    double fuseEffectStart{0.0};
    std::uint32_t hoveredHotspot{0};
    std::optional<SDL_Rect> hoveredBounds;
    int mouseX{fgp::kLogicalWidth / 2};
    int mouseY{fgp::kLogicalHeight / 2};
    bool mouseInsideWindow{true};
    CursorKind activeCursor{CursorKind::Default};
    std::chrono::steady_clock::time_point cursorAnimationStart;
    std::optional<std::pair<std::string, std::string>> pendingScene;
    std::optional<std::string> sceneReadyAfterPresent;
    int pendingSceneEntryMode{0};
    int activeSceneEntryMode{0};
    std::optional<std::pair<double, std::string>> sceneTimedTransition;
    bool developmentMode{false};
    std::vector<std::string> developerCommands;
    std::size_t developerCommandIndex{0};
    std::optional<std::uint32_t> developerHotspotOverride;
    bool developerConsoleRequested{false};
    double developerExitAfter{-1.0};
    std::chrono::steady_clock::time_point developerRunStart;
    // The epoch of the original's application clock 0x0040c450: timeGetTime()
    // minus [0x0054b8f0], which the first release of the pause gate sets at
    // launch. Gameplay that reads that clock (Letter Sort's per-second timer)
    // measures from here, not from developerRunStart, so a developer feature
    // that resets its own epoch cannot shift a scene's pacing.
    std::chrono::steady_clock::time_point applicationClockStart;
    std::optional<std::chrono::steady_clock::time_point> developerSleepStartedAt;
    DialogControl dialogPressedControl{DialogControl::None};
    int dialogPressedDay{0};
    std::string pendingProfileName;
    int pendingBirthMonth{0};
    int pendingBirthDay{1};
    std::string dialogTitle;
    std::string dialogMessage;
    MessageKind dialogMessageKind{MessageKind::Notice};
    // A scene already traced the menu's scene change (birthday's awards
    // open after it), so enterMenu does not trace it again.
    bool menuSceneChangeTraced{false};
    std::string awardTitle;
    std::string awardDescription;
    TexturePtr awardIconTexture;
    int awardIconWidth{0};
    int awardIconHeight{0};
    std::string awardIconTraceAsset;
    TexturePtr awardMedalTexture;
    int awardMedalWidth{0};
    int awardMedalHeight{0};
    std::string awardMedalTraceAsset;
    std::optional<std::uint32_t> pendingFactCompletionAward;
    std::vector<HelpPage> helpPages;
    std::size_t helpTopicIndex{0};
    std::size_t helpFirstVisibleTopic{0};
    int helpBodyScrollLine{0};
    bool helpScrollDragging{false};
    int helpScrollDragOffsetY{0};
    double helpFrozenElapsed{0.0};
    std::vector<std::string> profiles;
    fs::path profileDirectory;
    std::size_t profileIndex{0};
};

} // namespace fgp
