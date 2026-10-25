#pragma once

// The native game application: original state, views, and behaviors
// recovered from FGP.exe and the packaged assets. One instance per process;
// construct with the command line, then call run().
//
// Shared state lives in AppCore (app_core.h). Each minigame, and the story
// scenes that have been split out so far, owns its state and logic in its
// own class and files (app_<name>.h / app_<name>.cpp); GameApp combines them
// and dispatches the frame, input, scene loading and developer commands
// across them.

#include "game/app_activitypage.h"
#include "game/app_audiopairs.h"
#include "game/app_balloonhunt.h"
#include "game/app_beemaze.h"
#include "game/app_birthday.h"
#include "game/app_bouquet.h"
#include "game/app_credits.h"
#include "game/app_bugzzz.h"
#include "game/app_buildabike.h"
#include "game/app_buildacar.h"
#include "game/app_butterfly.h"
#include "game/app_buttercol.h"
#include "game/app_chess.h"
#include "game/app_colorfill.h"
#include "game/app_core.h"
#include "game/app_desk.h"
#include "game/app_goodbye.h"
#include "game/app_hopscotch.h"
#include "game/app_house.h"
#include "game/app_draughts.h"
#include "game/app_hustle.h"
#include "game/app_info.h"
#include "game/app_fence.h"
#include "game/app_puzzle.h"
#include "game/app_racemenu.h"
#include "game/app_racing.h"
#include "game/app_intent.h"
#include "game/app_intro.h"
#include "game/app_jigsaw.h"
#include "game/app_lettersort.h"
#include "game/app_mahjong.h"
#include "game/app_mainscore.h"
#include "game/app_mathlab.h"
#include "game/app_minigame_recovery.h"
#include "game/app_morning.h"
#include "game/app_mountain.h"
#include "game/app_outro.h"
#include "game/app_outtent.h"
#include "game/app_pee.h"
#include "game/app_pipeline.h"
#include "game/app_sliding.h"
#include "game/app_sockdrawer.h"
#include "game/app_synonym.h"
#include "game/app_textinvader.h"
#include "game/app_tournament.h"
#include "game/app_town.h"
#include "game/app_trivia.h"
#include "game/app_tvroom.h"
#include "game/app_tvstation.h"
#include "game/app_viewers.h"
#include "game/app_whackamole.h"
#include "game/app_wheelbarrow.h"
#include "game/app_yard.h"

namespace fgp {

class GameApp final : private IntroScene,
                      private YardScene,
                      private DeskScene,
                      private PeeScene,
                      private HouseScene,
                      private TvroomScene,
                      private IntentScene,
                      private OuttentScene,
                      private MorningScene,
                      private TownScene,
                      private TvstationScene,
                      private GoodbyeScene,
                      private OutroScene,
                      private RecoveredMinigameScene,
                      private WhackamoleScene,
                      private BalloonhuntScene,
                      private PipelineScene,
                      private HopscotchScene,
                      private BeemazeScene,
                      private BouquetScene,
                      private BirthdayScene,
                      private CreditsScene,
                      private BugzzzScene,
                      private WheelbarrowScene,
                      private BuildabikeScene,
                      private MountainScene,
                      private HustleScene,
                      private ChessScene,
                      private JigsawScene,
                      private SlidingScene,
                      private FenceScene,
                      private PuzzleScene,
                      private TextinvaderScene,
                      private SynonymScene,
                      private MathlabScene,
                      private MainscoreScene,
                      private TournamentScene,
                      private TriviaScene,
                      private BuildacarScene,
                      private AudiopairsScene,
                      private SockdrawerScene,
                      private ButterflyScene,
                      private LettersortScene,
                      private MahjongScene,
                      private ColorfillScene,
                      private DraughtsScene,
                      private ActivitypageScene,
                      private RacemenuScene,
                      private RacingScene,
                      private ButtercolScene,
                      private ViewerScenes {
public:
    GameApp(int argc, char **argv);
    int run();
    bool step();
    void shutdown();

private:
    void handleEvents();
    void renderFrame();
    void drawLoadingOverlay(double progress);
    void updateWebControls();
    void enterMenu(bool restartMusic = true) override;
    void loadScene(const std::string &sceneName, std::string_view label,
                   int entryMode);
    void configureStoryNavigation(const std::string &sceneName);
    bool executeDeveloperCommand(std::string_view command);
    void handleSceneAnimationFinished(std::string_view name,
                                      double completedAt);
    // A foreground clip began playing: the original's AnimStarted event
    // 0x10f, which 0x004129d0 posts through 0x0040c860 when a player starts.
    void handleSceneAnimationStarted(std::string_view name, double startedAt);
    void closeDialog();
    void startTournamentRound(int difficulty) override;
    // Escape in a scene: only the scenes whose own handler tests VK_ESCAPE
    // act on it; it is not a back key (see REVERSE_ENGINEERING.md).
    void handleSceneEscape();
    // The browser build's Back button: the scene's exit control, or Escape
    // where a dialog, the menu or Options is in front.
    void performWebBack();
    DialogControl dialogControlAt(int x, int y);
    // A screen point in the coordinates the dialog was drawn in: in the race
    // the dialog is drawn 80, 60 to the left of and above the 640x480 frame
    // that is stretched by 1.25.
    [[nodiscard]] bool raceDialogShown() const {
        return viewMode == ViewMode::Scene && activeScene == "racing" && racingGame &&
               racingGame->target;
    }
    [[nodiscard]] int dialogPointX(int x) const {
        return raceDialogShown() ? x * 640 / 800 + 80 : x;
    }
    [[nodiscard]] int dialogPointY(int y) const {
        return raceDialogShown() ? y * 480 / 600 + 60 : y;
    }
    std::optional<std::pair<std::uint32_t, SDL_Rect>> sceneDynamicHotspotAt(int x, int y);
    std::uint32_t hotspotAt(int x, int y) override;
    std::optional<SDL_Rect> currentHotspotBounds(
        std::uint32_t hotspot) override;
    // Browser build: the pending scene's data is still being fetched, so the
    // loading overlay is shown and input is held back.
    bool sceneDataLoading{false};
    // Browser build: a visitor without a profile is asked for one once, on
    // first reaching the menu.
    bool askedForWebProfile{false};
    // The race's dialogs, drawn at 800x600 and laid over its 640x480 frame.
    TexturePtr raceDialogTarget;
    // Developer "screenshot:PATH": the next presented frame, saved as BMP.
    std::string developerScreenshotPath;
};

} // namespace fgp
