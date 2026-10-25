#include "game/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "assets/profile.h"
#include "game/menu_names.h"
#include "generated/scene_navigations.hpp"
#include "media/audio.h"
#include "media/smacker.h"
#include "platform/leaderboard.h"
#include "platform/paths.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {

void AppCore::showAward(std::string_view iconAsset, std::string_view titleKey,
                        std::string_view descriptionKey, bool traceIcon) {
    showAwardText(iconAsset, language.get("awarddlg", titleKey, "Premie"),
                  language.get("awarddlg", descriptionKey,
                               "Du har fått en premie."),
                  traceIcon);
}

void AppCore::showAwardText(std::string_view iconAsset, std::string title,
                            std::string description, bool traceIcon) {
    awardMedalTexture.reset();
    awardMedalWidth = 0;
    awardMedalHeight = 0;
    awardMedalTraceAsset.clear();
    awardIconTexture.reset();
    awardIconWidth = 0;
    awardIconHeight = 0;
    awardIconTraceAsset.clear();
    // An empty asset is a medal award with no car part (0x0041bd89 skips
    // the part loop for a zero count).
    if (!iconAsset.empty()) {
        SurfacePtr icon = loadGreenKeyedBmp(
            common.data(common.find(std::string(iconAsset))));
        awardIconWidth = icon->w;
        awardIconHeight = icon->h;
        awardIconTexture = makeTexture(renderer, icon.get());
        SDL_SetTextureBlendMode(awardIconTexture.get(), SDL_BLENDMODE_BLEND);
        awardIconTraceAsset = "../../common/" + std::string(iconAsset);
        std::replace(awardIconTraceAsset.begin(), awardIconTraceAsset.end(),
                     '\\', '/');
        if (awardIconTraceAsset.ends_with(".bmp")) {
            awardIconTraceAsset.resize(awardIconTraceAsset.size() - 4U);
        }
    }
    awardTitle = std::move(title);
    awardDescription = std::move(description);
    awardDescription += "\n\n";
    awardDescription += language.get(
        "awarddlg", "AWARDBELOW", "Sjekk under hva du har fått i premie.");
    dialogMode = DialogMode::Award;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    referenceTrace.emit("animation_add", {
        {"asset", "../../common/dialogue/awarddlg/backdrop"},
        {"x", 241}, {"y", 127}, {"layer", 500}, {"hotspot", 0},
        {"conflict_mask", 0}});
    if (traceIcon && !awardIconTraceAsset.empty()) {
        referenceTrace.emit("animation_add", {
            {"asset", awardIconTraceAsset},
            {"x", -1}, {"y", -1}, {"layer", 501}, {"hotspot", 550},
            {"conflict_mask", 0}});
    }
    SDL_StopTextInput();
    SDL_SetWindowTitle(window, "FGP Native — Premie");
}

void AppCore::showMedalAward(std::uint32_t partNumber,
                             std::uint32_t medalLevel) {
    static constexpr std::array<std::string_view, 5> medalNames{
        "none", "bronze", "silver", "gold", "platinum"};
    static constexpr std::array<std::string_view, 5> titleKeys{
        "", "TITLE_BRONZE", "TITLE_SILVER", "TITLE_GOLD",
        "TITLE_PLATINUM"};
    static constexpr std::array<std::string_view, 5> descriptionKeys{
        "", "DESC_BRONZE", "DESC_SILVER", "DESC_GOLD",
        "DESC_PLATINUM"};
    if (medalLevel == 0U || medalLevel >= medalNames.size()) {
        throw std::runtime_error("Invalid medal award level");
    }

    // Part 0: no part granted (0x0041f9d0's medals, or all 62 owned).
    std::string partAsset;
    if (partNumber > 0U) {
        std::string number = std::to_string(partNumber);
        if (number.size() < 2) {
            number.insert(number.begin(), '0');
        }
        partAsset = "bitmap\\award\\part_" + number + ".bmp";
    }
    showAward(partAsset, titleKeys[medalLevel], descriptionKeys[medalLevel],
              false);
    const std::string medalAsset =
        "bitmap\\award\\" + std::string(medalNames[medalLevel]) + ".bmp";
    SurfacePtr medal = loadGreenKeyedBmp(
        common.data(common.find(medalAsset)));
    awardMedalWidth = medal->w;
    awardMedalHeight = medal->h;
    awardMedalTexture = makeTexture(renderer, medal.get());
    SDL_SetTextureBlendMode(awardMedalTexture.get(), SDL_BLENDMODE_BLEND);
    awardMedalTraceAsset = "../../common/bitmap/award/" +
                           std::string(medalNames[medalLevel]);
    referenceTrace.emit("animation_add", {
        {"asset", awardMedalTraceAsset},
        {"x", -1}, {"y", -1}, {"layer", 501}, {"hotspot", 554},
        {"conflict_mask", 0}});
    if (!awardIconTraceAsset.empty()) {
        referenceTrace.emit("animation_add", {
            {"asset", awardIconTraceAsset},
            {"x", -1}, {"y", -1}, {"layer", 501}, {"hotspot", 550},
            {"conflict_mask", 0}});
    }
}

void AppCore::showBonusCarPartAward(std::uint32_t partNumber) {
    // 0x0041bb20(0, 0, 0, parts, 0): no caption and no medal, so
    // 0x0041be20 takes TITLE_DEFAULT (0x0041be58) and type 0's
    // DESC_CARPART (jump table 0x0041bfdc).
    std::string number = std::to_string(partNumber);
    if (number.size() < 2) {
        number.insert(number.begin(), '0');
    }
    showAward("bitmap\\award\\part_" + number + ".bmp", "TITLE_DEFAULT",
              "DESC_CARPART");
}

void AppCore::showCarPartAward(std::uint32_t partNumber) {
    std::string number = std::to_string(partNumber);
    if (number.size() < 2) {
        number.insert(number.begin(), '0');
    }
    showAward("bitmap\\award\\part_" + number + ".bmp",
              "TITLE_HIDDEN", "DESC_HIDDEN");
}

void AppCore::showFactCompletionAward(std::uint32_t partNumber) {
    std::string number = std::to_string(partNumber);
    if (number.size() < 2) {
        number.insert(number.begin(), '0');
    }
    showAward("bitmap\\award\\part_" + number + ".bmp",
              "TITLE_FACT", "DESC_FACT");
}

void AppCore::showAnaglyphAward() {
    showAward("bitmap\\award\\filmclip.bmp",
              "TITLE_3DSCENE", "DESC_3DSCENE");
}

void GameApp::closeDialog() {
    // 0x00419660 returns into the game's init, which starts play at once.
    const bool startTournamentPlay =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::TournamentPlayerReady &&
        tournament.active && !tournament.players.empty();
    if (startTournamentPlay) {
        dialogMode = DialogMode::None;
        dialogPressedControl = DialogControl::None;
        startTournamentRound(
            tournament.players[static_cast<std::size_t>(tournament.player)]
                .difficulty);
        return;
    }
    const bool closingHelp = dialogMode == DialogMode::Help;
    // The race's crash box (0x00421310) returns into 0x004770c0, the end.
    const bool endCrashedRace = dialogMode == DialogMode::Message &&
                                dialogMessageKind == MessageKind::RacingCrashed;
    const bool closingFrozenClock = dialogClockFrozen();
    const bool closingHighScore = dialogMode == DialogMode::HighScore;
    const bool closingAward = dialogMode == DialogMode::Award;
    const bool continueHighScoreAfterAward =
        dialogMode == DialogMode::Award && pendingHighScoreAfterAward;
    const bool advanceWhackamoleLevel =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::WhackamoleNextLevel &&
        viewMode == ViewMode::Scene && activeScene == "whackamole" &&
        whackamoleGame.has_value();
    const bool showWhackamoleScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::WhackamoleGameOver &&
        viewMode == ViewMode::Scene && activeScene == "whackamole" &&
        whackamoleGame.has_value();
    const bool showBalloonhuntScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::BalloonhuntGameOver &&
        viewMode == ViewMode::Scene && activeScene == "balloonhunt" &&
        balloonhuntGame.has_value();
    const bool showButterflyScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::ButterflyGameOver &&
        viewMode == ViewMode::Scene && activeScene == "butterfly" &&
        butterflyGame.has_value();
    const bool showAudiopairsScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::AudiopairsGameOver &&
        viewMode == ViewMode::Scene && activeScene == "audiopairs" &&
        audiopairsGame.has_value();
    const bool showSockdrawerScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::SockdrawerGameOver &&
        viewMode == ViewMode::Scene && activeScene == "sockdrawer" &&
        sockdrawerGame.has_value();
    const bool showBuildabikeScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::BuildabikeGameOver &&
        viewMode == ViewMode::Scene && activeScene == "buildabike" &&
        buildabikeGame.has_value();
    const bool continueBeemazeGameOver =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::BeemazeGameOver &&
        viewMode == ViewMode::Scene && activeScene == "beemaze" &&
        beemazeGame.has_value();
    const bool showHustleScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::HustleGameOver &&
        viewMode == ViewMode::Scene && activeScene == "hustle" &&
        hustleGame.has_value();
    const bool showHopscotchScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::HopscotchGameOver &&
        viewMode == ViewMode::Scene && activeScene == "hopscotch" &&
        hopscotchGame.has_value();
    const bool showTriviaScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::TriviaGameOver &&
        viewMode == ViewMode::Scene && activeScene == "gametrivia" &&
        triviaGame.has_value();
    const bool showLettersortScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::LettersortGameOver &&
        viewMode == ViewMode::Scene && activeScene == "lettersort" &&
        lettersortGame.has_value();
    const bool showWheelbarrowScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::WheelbarrowGameOver &&
        viewMode == ViewMode::Scene && activeScene == "wheelbarrow" &&
        wheelbarrowGame.has_value();
    const bool showTextinvaderScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::TextinvaderGameOver &&
        viewMode == ViewMode::Scene && activeScene == "textinvader" &&
        textinvaderGame.has_value();
    const bool continuePuzzleGameOver =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::PuzzleGameOver &&
        viewMode == ViewMode::Scene && activeScene == "puzzle" &&
        puzzleGame.has_value();
    const bool continuePipelineLevel =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::PipelineLevelComplete &&
        viewMode == ViewMode::Scene && activeScene == "pipeline" &&
        pipelineGame.has_value();
    const bool showPipelineScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::PipelineGameOver &&
        viewMode == ViewMode::Scene && activeScene == "pipeline" &&
        pipelineGame.has_value();
    const bool showMathlabScore =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::MathlabGameOver &&
        viewMode == ViewMode::Scene && activeScene == "mathlab" &&
        mathlabGame.has_value();
    // Draughts' modal boxes (0x00421310) return into 0x004650a2 (Game
    // Over) and 0x00464ac2 (abort declined, also by Escape).
    const bool continueDraughts =
        dialogMode == DialogMode::Message &&
        (dialogMessageKind == MessageKind::DraughtsGameOver ||
         dialogMessageKind == MessageKind::DraughtsAbortConfirmation) &&
        viewMode == ViewMode::Scene && activeScene == "draughts" &&
        draughtsGame.has_value();
    const bool draughtsGameOver =
        dialogMessageKind == MessageKind::DraughtsGameOver;
    const bool restoreSynonymStart =
        closingHighScore && viewMode == ViewMode::Scene &&
        activeScene == "synonym" && synonymGame.has_value();
    const int whackamoleScore =
        showWhackamoleScore ? whackamoleGame->score : 0;
    const int balloonhuntScore =
        showBalloonhuntScore ? balloonhuntGame->score : 0;
    const int butterflyScore =
        showButterflyScore ? butterflyGame->finalScore : 0;
    const int audiopairsScore =
        showAudiopairsScore ? audiopairsGame->finalScore : 0;
    const int sockdrawerScore =
        showSockdrawerScore ? sockdrawerGame->finalScore : 0;
    const int buildabikeScore =
        showBuildabikeScore ? buildabikeGame->score : 0;
    const int hustleScore =
        showHustleScore ? hustleGame->finalBananas : 0;
    const int lettersortScore =
        showLettersortScore ? lettersortGame->score : 0;
    const int wheelbarrowScore =
        showWheelbarrowScore ? wheelbarrowGame->score : 0;
    const int textinvaderScore =
        showTextinvaderScore ? textinvaderGame->score : 0;
    const int pipelineScore = showPipelineScore ? pipelineGame->score : 0;
    const int mathlabScore =
        showMathlabScore ? mathlabGame->shownScore : 0;
    // FGP.exe's modal call at 0x0044EB9B blocks; when it returns, execution
    // falls through the bonus calculation and jumps to 0x0044DF80. Native
    // dialogs are asynchronous, so every dismissal path rejoins here instead.
    const bool completedFactPage =
        dialogMode == DialogMode::Message &&
        dialogMessageKind == MessageKind::Fact &&
        pendingFactCompletionAward.has_value();
    const auto factAward = pendingFactCompletionAward;
    if (completedFactPage) {
        pendingFactCompletionAward.reset();
    }
    if (closingHighScore) {
        referenceTrace.emit("animation_remove", {
            {"asset", "../../common/dialogue/highscore/backdrop"},
            {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", "../../common/dialogue/highscore/backdrop"},
            {"reason", "stopped"}});
    }
    if (closingAward) {
        for (const std::string *asset :
             {&awardIconTraceAsset, &awardMedalTraceAsset}) {
            if (!asset->empty()) {
                referenceTrace.emit("animation_remove", {
                    {"asset", *asset}, {"reason", "removed"}});
                referenceTrace.emit("animation_stop", {
                    {"asset", *asset}, {"reason", "stopped"}});
            }
        }
        referenceTrace.emit("animation_remove", {
            {"asset", "../../common/dialogue/awarddlg/backdrop"},
            {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", "../../common/dialogue/awarddlg/backdrop"},
            {"reason", "stopped"}});
    }
    dialogMode = DialogMode::None;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    helpScrollDragging = false;
    if (closingFrozenClock && !paused) {
        start = std::chrono::steady_clock::now() -
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(helpFrozenElapsed));
    }
    if (endCrashedRace) {
        finishCrashedRace();
        return;
    }
    if (closingHelp) {
        helpPages.clear();
        helpTopicIndex = 0;
        helpFirstVisibleTopic = 0;
        helpBodyScrollLine = 0;
    }
    SDL_StopTextInput();
    if (viewMode == ViewMode::Scene) {
        const std::string title =
            "FGP Native — " + activeScene + " · exit control back";
        SDL_SetWindowTitle(window, title.c_str());
    } else {
        setMenuTitle();
    }
    if (continueDraughts) {
        if (draughtsGameOver) {
            finishDraughtsGameOver();
        } else {
            finishDraughtsAbort(false);
        }
    }
    if (closingAward && birthdayGame && birthdayGame->leaving) {
        continueBirthdayLeave();
    } else if (continueHighScoreAfterAward) {
        pendingHighScoreAfterAward = false;
        std::uint32_t bonusPart = 0U;
        if (pendingBonusCarParts > 0U &&
            profileIndex < profiles.size() &&
            profiles[profileIndex] != "No profile") {
            try {
                bonusPart = awardMedalCarParts(
                    profileDirectory / (profiles[profileIndex] + ".pfl"),
                    pendingBonusCarParts);
            } catch (const std::exception &error) {
                std::cerr << "Could not record bonus car parts: "
                          << error.what() << '\n';
            }
        }
        pendingBonusCarParts = 0U;
        if (bonusPart > 0U) {
            showBonusCarPartAward(bonusPart);
            pendingHighScoreAfterAward = true;
        } else {
            persistHighScore();
            openHighScoreDialog();
        }
    } else if (completedFactPage) {
        showFactCompletionAward(*factAward);
    } else if (advanceWhackamoleLevel) {
        beginWhackamoleLevel();
    } else if (showWhackamoleScore) {
        showHighScore(whackamoleScore);
    } else if (showBalloonhuntScore) {
        showHighScore(balloonhuntScore);
    } else if (showButterflyScore) {
        showHighScore(butterflyScore);
    } else if (showAudiopairsScore) {
        showHighScore(audiopairsScore);
    } else if (showSockdrawerScore) {
        showHighScore(sockdrawerScore);
    } else if (showBuildabikeScore) {
        // 0x00438d62 calls the shared score wrapper once the modal returns.
        showHighScore(buildabikeScore);
    } else if (continueBeemazeGameOver) {
        continueBeemazeAfterGameOverMessage();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "beemaze" && beemazeGame) {
        restoreBeemazeStartAfterHighScore();
    } else if (showHustleScore) {
        showHighScore(hustleScore);
    } else if (showHopscotchScore) {
        // 0x0043cdbd-0x0043cdec: back to Start, then the score wrapper.
        returnHopscotchToAttract();
        showHighScore(hopscotchGame->score);
    } else if (showTriviaScore) {
        // 0x0047fb3e-0x0047fb4b: the score through 0x0041f9d0.
        showHighScore(triviaGame->score);
    } else if (showLettersortScore) {
        // 0x43fd27-0x43fd3f: the score table follows the GAMEOVER message.
        showHighScore(lettersortScore);
    } else if (showWheelbarrowScore) {
        // 0x0045022c-0x0045023a: the score wrapper, labelled LONGNAME,
        // follows the GAMEOVER message.
        showHighScore(wheelbarrowScore, "LONGNAME");
    } else if (showTextinvaderScore) {
        // 0x0044d4a9-0x0044d4b8: the score table follows the GAMEOVER message.
        showHighScore(textinvaderScore);
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "textinvader" && textinvaderGame) {
        finishTextinvaderGameOver();
    } else if (continuePipelineLevel) {
        // 0x0044a6d4 returns into the next level's setup.
        continuePipelineAfterLevelMessage();
    } else if (showPipelineScore) {
        // 0x0044a800: the score wrapper follows the GAMEOVER message.
        showHighScore(pipelineScore);
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "pipeline" && pipelineGame) {
        finishPipelineGameOver();
    } else if (showMathlabScore) {
        // 0x00482143: the score wrapper follows the GAMEOVER message.
        showHighScore(mathlabScore);
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "whackamole" && whackamoleGame) {
        referenceTrace.emit("animation_add", {
            {"asset", "startbuttn"}, {"x", -1}, {"y", -1},
            {"layer", 200}, {"hotspot", 34}, {"conflict_mask", 0}});
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "balloonhunt" && balloonhuntGame) {
        returnToBalloonhuntMenu();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "butterfly" && butterflyGame) {
        // FGP.exe callback 0x0043bb90 removes timerend, reinstalls the idle
        // timer object, adds startanim at layer 150, and starts it.
        auto &game = *butterflyGame;
        referenceTrace.emit("animation_remove", {
            {"asset", "timerend"}, {"reason", "removed"}});
        referenceTrace.emit("animation_stop", {
            {"asset", "timerend"}, {"reason", "stopped"}});
        referenceTrace.emit("animation_add", {
            {"asset", "timerstart"}, {"x", -1}, {"y", -1},
            {"layer", 0}, {"hotspot", 0}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_add", {
            {"asset", "startanim"}, {"x", -1}, {"y", -1},
            {"layer", 150}, {"hotspot", 1}, {"conflict_mask", 0}});
        referenceTrace.emit("animation_start", {
            {"asset", "startanim"}, {"x", -1}, {"y", -1},
            {"layer", 150}, {"loop", true}});
        game.phase = ButterflyPhase::Attract;
        game.entities.clear();
        game.caught.fill(0);
        game.score = 0;
        game.finalScore = 0;
        game.levelScore = 0;
        game.level = 0;
        game.catcherSwingStarted = -1.0;
        game.attractStartedAt = sceneElapsedNow();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "audiopairs" && audiopairsGame) {
        // FGP.exe 0x004296df-0x004296f3 invokes the `start` descriptor after
        // the shared score wrapper returns. The captured descriptor reports
        // its packaged position, layer and hotspot at this point.
        auto &game = *audiopairsGame;
        referenceTrace.emit("animation_add", {
            {"asset", "start"}, {"x", 556}, {"y", 128},
            {"layer", 50}, {"hotspot", 3}, {"conflict_mask", 0}});
        game.phase = AudiopairsPhase::Attract;
        game.firstSelection.reset();
        game.secondSelection.reset();
        game.points = 0;
        game.finalScore = 0;
        game.activeAnimation = 0U;
        game.activeAnimationStarted = sceneElapsedNow();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "sockdrawer" && sockdrawerGame) {
        // FGP.exe passes callback 0x0044C3E0 to the shared score wrapper.
        // It closes the live board through 0x0044BAB0 and adds `start` at
        // layer -10; the score itself is not cleared until hotspot 3 starts
        // another round at 0x0044C100-0x0044C11F.
        returnToSockdrawerAttract();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "buildabike" && buildabikeGame) {
        finishBuildabikeGameOver();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "mathlab" && mathlabGame) {
        finishMathlabGameOver();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "lettersort" && lettersortGame) {
        // Score-table callback 0x43fd50.
        returnToLettersortAttract();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "wheelbarrow" && wheelbarrowGame) {
        finishWheelbarrowGameOver();
    } else if (continuePuzzleGameOver) {
        continuePuzzleAfterGameOverMessage();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "puzzle" && puzzleGame) {
        continuePuzzleAfterHighScore();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "gametrivia" && triviaGame) {
        returnTriviaToAttract();
    } else if (closingHighScore && viewMode == ViewMode::Scene &&
               activeScene == "mountain" && mountainGame) {
        // 0x00447e43's blocking score call returned.
        finishMountainGameOver();
    } else if (restoreSynonymStart) {
        synonymGame->phase = SynonymPhase::Attract;
        synonymGame->showAttractResult = true;
        synonymGame->prompt.clear();
        referenceTrace.emit("animation_add", {
            {"asset", "start"}, {"x", -1}, {"y", -1},
            {"layer", 50}, {"hotspot", 1}, {"conflict_mask", 0}});
    }
}

ScoreProfiles AppCore::scoreProfiles() const {
    ScoreProfiles result;
    result.directory = profileDirectory;
    for (const auto &name : profiles) {
        if (name != "No profile") {
            result.names.push_back(name);
        }
    }
    if (profileIndex < profiles.size() && profiles[profileIndex] != "No profile" &&
        fs::exists(profileDirectory / (profiles[profileIndex] + ".pfl"))) {
        result.current = profiles[profileIndex];
    }
    return result;
}

void AppCore::fillHighScoreRows(const ScoreLists &lists) {
    const auto rows = [](const ScoreList &list) {
        std::vector<HighScoreRow> result;
        for (const auto &row : list.rows) {
            result.push_back(
                {row.player, formatScoreDate(row.timestamp), row.score});
        }
        return result;
    };
    highScoreProfileRows = rows(lists.profile);
    highScoreGlobalRows = rows(lists.global);
    highScoreUnionRows = rows(lists.players);
}

void AppCore::showHighScore(int score, std::string_view titleKey,
                            std::uint32_t listKey, std::uint32_t flags) {
    // The shared scored-minigame wrapper at 0x0041F9A0 reaches the modal
    // high-score UI at 0x0041F910. 0x0041F360 creates the profile/global/
    // union tabs (controls 0xC9-0xCB) and close control 0xCC; 0x0041F968-
    // 0x0041F978 blocks until that close path returns 0x10.
    // 0x0041f8c0: in a tournament the score goes to the round's list and
    // play passes on; no dialog, medal or profile record.
    if (tournament.active) {
        recordTournamentScore(score);
        return;
    }
    highScoreTab = HighScoreTab::Profile;
    highScoreValue = score;
    highScoreListKey = listKey;
    highScoreFlags = flags;
    highScoreHasCurrentRow = true;
    highScoreMedalLevel = 0U;
    pendingHighScoreAfterAward = false;
    pendingBonusCarParts = 0U;
    // The caller's label key: most games pass <scene>:GAMENAME; Bugzzz
    // pushes "bugzzz:HISCORE" at 0x00434575.
    highScoreGameName = language.get(activeScene, std::string(titleKey), "");
    if (highScoreGameName.empty()) {
        highScoreGameName = language.get(
            activeScene, "SCENENAME", activeScene);
    }
    highScoreTimestamp =
        static_cast<std::uint32_t>(std::time(nullptr));  // 0x0041e90d
    highScoreDate = formatScoreDate(highScoreTimestamp);
    highScoreProfileRows.clear();
    highScoreGlobalRows.clear();
    highScoreUnionRows.clear();

    // 0x0041ee50 -> 0x0041eed0: every caller, whatever its scene, goes
    // through the same lists, grading and type-0x401 record. The thresholds
    // are the table the scene's call site pushes (medal_tables.hpp); a
    // NULL table (Puzzle) grades no medal but still records the score.
    const auto medalTable = sceneMedalTable(activeScene);
    const ScoreProfiles players = scoreProfiles();
    ScoreSubmission submission;
    try {
        submission = submitScore(
            players, activeScene, highScoreListKey,
            static_cast<std::uint32_t>(std::max(0, score)),
            medalTable ? &medalTable->thresholds : nullptr, flags,
            medalTable ? medalTable->awardsCarParts : true,
            highScoreTimestamp, executableGameData.scoredScenes());
    } catch (const std::exception &error) {
        std::cerr << "Could not record " << activeScene << " score: "
                  << error.what() << '\n';
    }
    highScoreMedalLevel = submission.medal;
    // 0x00411eb0: the active profile's name, or profile:NONAME.
    highScorePlayer = !submission.row.player.empty()
                          ? submission.row.player
                          : language.get("profile", "NONAME", "");
    fillHighScoreRows(submission.lists);
    // Browser build: the Global tab shows the shared leaderboard instead.
    if ((flags & 0xff00U) != kScoreLowFirst) {
        fillSharedGlobalRows();
    }

    if (players.current) {
        const fs::path scoreProfile =
            profileDirectory / (*players.current + ".pfl");
        // 0x0041ef2c: the medal award, with one car part per level gained
        // (none through 0x0041f9d0), then 0x0041ef6c: the parts for raising
        // the lowest medal over every scored scene.
        try {
            if (submission.bonusCarParts > 0U) {
                pendingBonusCarParts = submission.bonusCarParts;
            }
            if (submission.medal > submission.previousMedal) {
                const std::uint32_t partNumber =
                    awardMedalCarParts(scoreProfile, submission.medalCarParts);
                showMedalAward(partNumber, submission.medal);
                pendingHighScoreAfterAward = true;
                return;
            }
            if (pendingBonusCarParts > 0U) {
                const std::uint32_t partNumber =
                    awardMedalCarParts(scoreProfile, pendingBonusCarParts);
                pendingBonusCarParts = 0U;
                if (partNumber > 0U) {
                    showBonusCarPartAward(partNumber);
                    pendingHighScoreAfterAward = true;
                    return;
                }
            }
        } catch (const std::exception &error) {
            std::cerr << "Could not record " << activeScene
                      << " medal award in " << scoreProfile << ": "
                      << error.what() << '\n';
        }
    }
    persistHighScore();
    openHighScoreDialog();
}

ScoreRow AppCore::submitSceneScore(std::uint32_t score, std::uint32_t listKey,
                                   std::uint32_t flags) {
    const auto timestamp = static_cast<std::uint32_t>(std::time(nullptr));
    try {
        // A NULL table: no medal, so no award (0x0041efe0 returns 0).
        return submitScore(scoreProfiles(), activeScene, listKey, score,
                           nullptr, flags, true, timestamp)
            .row;
    } catch (const std::exception &error) {
        std::cerr << "Could not record " << activeScene << " score: "
                  << error.what() << '\n';
    }
    return ScoreRow{"", timestamp, score};
}

void AppCore::showScoreTable(std::string_view scene, std::string caption,
                             std::uint32_t listKey, std::uint32_t flags,
                             const std::optional<ScoreRow> &currentRow) {
    // 0x0041f030: 0x0041ebd0 fills the lists, 0x0041f230 builds the dialog
    // and, given a current row (its fourth argument), 0x0041f0e0 boxes it;
    // then the same modal loop as 0x0041f910.
    highScoreTab = HighScoreTab::Profile;
    highScoreListKey = listKey;
    highScoreFlags = flags;
    highScoreHasCurrentRow = currentRow.has_value();
    highScoreValue = currentRow ? static_cast<int>(currentRow->score) : 0;
    highScoreDate = currentRow ? formatScoreDate(currentRow->timestamp) : "";
    highScorePlayer = currentRow ? currentRow->player : "";
    if (currentRow && highScorePlayer.empty()) {
        highScorePlayer = language.get("profile", "NONAME", "");
    }
    highScoreMedalLevel = 0U;
    pendingHighScoreAfterAward = false;
    pendingBonusCarParts = 0U;
    // 0x0041f2a0: a NULL caption takes the active scene's title
    // (0x0040ce70).
    highScoreGameName = !caption.empty()
                            ? std::move(caption)
                            : language.get(activeScene, "SCENENAME", activeScene);
    ScoreLists lists;
    try {
        lists = loadScoreLists(scoreProfiles(), scene, listKey, flags);
    } catch (const std::exception &error) {
        std::cerr << "Could not read " << scene << " scores: " << error.what()
                  << '\n';
    }
    fillHighScoreRows(lists);
    openHighScoreDialog();
}

// Browser build: the original dialog's Global tab lists the best scores of
// every player on every device, from the shared leaderboard
// (src/platform/leaderboard). The score just finished is included even
// before the server has stored it. Desktop builds keep the original rows.
void AppCore::fillSharedGlobalRows() {
    auto entries = leaderboardEntries(activeScene);
    if (!entries) {
        return;
    }
    std::vector<HighScoreRow> rows;
    for (const auto &entry : *entries) {
        std::string date;
        if (entry.date.size() >= 10) {  // YYYY-MM-DD... -> DD.MM.YY
            date = entry.date.substr(8, 2) + "." + entry.date.substr(5, 2) + "." +
                   entry.date.substr(2, 2);
        }
        rows.push_back({utf8ToCp1252(entry.player).value_or("?"), date, entry.score});
    }
    const bool hasProfile =
        profileIndex < profiles.size() && profiles[profileIndex] != "No profile";
    const std::string player = hasProfile ? profiles[profileIndex] : "Anonym";
    const auto score = static_cast<std::uint32_t>(std::max(0, highScoreValue));
    const bool listed = std::any_of(rows.begin(), rows.end(), [&](const HighScoreRow &row) {
        return row.player == player && row.score == score;
    });
    if (!listed && score > 0) {
        rows.push_back({player, highScoreDate, score});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const HighScoreRow &left, const HighScoreRow &right) {
                         return left.score > right.score;
                     });
    if (rows.size() > 10) {
        rows.resize(10);
    }
    highScoreGlobalRows = std::move(rows);
}

void AppCore::persistHighScore() {
    // The profile record is written by submitScore (0x0041eed0) before the
    // dialog opens. Browser build: every finished scored game also goes to
    // the shared leaderboard; players without a profile are listed as
    // "Anonym". Times (the race, the puzzles) do not: the leaderboard ranks
    // points high-first.
    if ((highScoreFlags & 0xff00U) == kScoreLowFirst) {
        return;
    }
    // The game's name sits under different keys: most games have SCENENAME,
    // Whack-a-Mole only GAMENAME, Bugzzz only TITLE.
    std::string gameTitle;
    for (const char *key : {"SCENENAME", "GAMENAME", "TITLE"}) {
        if (gameTitle.empty()) {
            gameTitle = language.get(activeScene, key, "");
        }
    }
    const bool hasProfile =
        profileIndex < profiles.size() && profiles[profileIndex] != "No profile";
    submitLeaderboardScore(activeScene, gameTitle.empty() ? activeScene : gameTitle,
                           hasProfile ? profiles[profileIndex] : "Anonym",
                           highScoreValue);
}

void AppCore::openHighScoreDialog() {
    // The dialogue centres itself in the logical display: (237,102) at
    // 800x600 and (157,42) in Mountain's 640x480 (0x004442bf).
    const bool smallDisplay = activeScene == "mountain";
    referenceTrace.emit("animation_add", {
        {"asset", "../../common/dialogue/highscore/backdrop"},
        {"x", smallDisplay ? 157 : 237}, {"y", smallDisplay ? 42 : 102},
        {"layer", 500}, {"hotspot", 0}, {"conflict_mask", 0}});
    dialogMode = DialogMode::HighScore;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StopTextInput();
    SDL_SetWindowTitle(window, "FGP Native — Poengtavle");
}

void AppCore::showMessage(std::string message, MessageKind kind) {
    showMessageWithTitle(
        language.get("interfaceh", "WARNING", "Advarsel"),
        std::move(message), kind);
}

void AppCore::showMessageWithTitle(std::string messageTitle, std::string message,
                                   MessageKind kind) {
    dialogTitle = std::move(messageTitle);
    dialogMessage = std::move(message);
    dialogMessageKind = kind;
    dialogMode = DialogMode::Message;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StopTextInput();
    const std::string windowTitle = "FGP Native — " + dialogTitle;
    SDL_SetWindowTitle(window, windowTitle.c_str());
}

void AppCore::showExitConfirmation() {
    showMessage(language.get(
                    "menu", "QUIT",
                    "Er du sikker på at du vil avslutte spillet?"),
                MessageKind::ExitConfirmation);
}

void AppCore::showMorningRideConfirmation() {
    // The shared message box 0x00420f40 freezes the game clock for its modal
    // loop (0x0040d270 at 0x00420f46, released at 0x0042104f): the captures
    // show no scene event while the question is up. Only this box does so
    // natively so far; the other message boxes still run the clock.
    helpFrozenElapsed = animationTimeNow();
    showMessage(language.get(
                    "morning", "RIDEDOWN",
                    "Ønsker du å hjelpe Solan på reisen ned Flåklypatoppen?"),
                MessageKind::MorningRideConfirmation);
}

void AppCore::openHelpPages(std::vector<HelpPage> pages) {
    helpFrozenElapsed = animationTimeNow();
    helpPages = std::move(pages);
    helpTopicIndex = 0;
    helpFirstVisibleTopic = 0;
    helpBodyScrollLine = 0;
    helpScrollDragging = false;
    helpScrollDragOffsetY = 0;
    dialogTitle = language.get("interfaceh", "HELP", "Hjelp");
    dialogMode = DialogMode::Help;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StopTextInput();
    // HELP_Run at 0x0041e247 calls the shared clock freeze before creating
    // its modal controls, then resumes it after the modal loop returns. It
    // makes no audio call, so the existing background playhead continues.
    SDL_SetWindowTitle(window, "FGP Native — Help");
}

void AppCore::selectHelpTopic(std::size_t index) {
    if (helpPages.empty()) {
        return;
    }
    helpTopicIndex = std::min(index, helpPages.size() - 1);
    helpBodyScrollLine = 0;
    const std::size_t visibleTopics = std::max<std::size_t>(
        1U, 292U / static_cast<std::size_t>(std::max(dialogFont.height, 1)));
    if (helpTopicIndex < helpFirstVisibleTopic) {
        helpFirstVisibleTopic = helpTopicIndex;
    } else if (helpTopicIndex >= helpFirstVisibleTopic + visibleTopics) {
        helpFirstVisibleTopic = helpTopicIndex - visibleTopics + 1U;
    }
}

std::vector<std::string> AppCore::helpBodyLines() const {
    std::vector<std::string> lines;
    if (helpPages.empty() || helpTopicIndex >= helpPages.size()) {
        return lines;
    }
    for (const auto &paragraph : helpPages[helpTopicIndex].paragraphs) {
        auto paragraphLines = wrapText(dialogFont, paragraph, 290);
        lines.insert(lines.end(), paragraphLines.begin(), paragraphLines.end());
    }
    return lines;
}

int AppCore::helpMaximumBodyScroll() const {
    const int visibleLines = std::max(1, 292 / std::max(dialogFont.height, 1));
    return std::max(0, static_cast<int>(helpBodyLines().size()) - visibleLines);
}

void AppCore::scrollHelpBody(int lines) {
    helpBodyScrollLine = std::clamp(
        helpBodyScrollLine + lines, 0, helpMaximumBodyScroll());
}

SDL_Rect AppCore::helpScrollThumbBounds() const {
    constexpr int dialogX = 158;
    constexpr int dialogY = 93;
    constexpr int trackY = 82;
    constexpr int trackHeight = 242;
    const int thumbHeight = helpThumb ? helpThumb->h : 29;
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int maximum = helpMaximumBodyScroll();
    const int offset = maximum > 0
                           ? static_cast<int>(std::lround(
                                 travel * helpBodyScrollLine /
                                 static_cast<double>(maximum)))
                           : 0;
    return SDL_Rect{dialogX + 450, dialogY + trackY + offset,
                    helpThumb ? helpThumb->w : 14, thumbHeight};
}

std::optional<std::size_t> AppCore::helpTopicAt(int x, int y) const {
    constexpr int dialogX = 158;
    constexpr int dialogY = 93;
    const int localX = x - dialogX;
    const int localY = y - dialogY;
    if (localX < 15 || localX >= 134 || localY < 56 || localY >= 348 ||
        dialogFont.height <= 0) {
        return std::nullopt;
    }
    const std::size_t row = static_cast<std::size_t>(
        (localY - 56) / dialogFont.height);
    const std::size_t index = helpFirstVisibleTopic + row;
    return index < helpPages.size() ? std::optional{index} : std::nullopt;
}

void AppCore::openNameDialog() {
    const bool placeholder = profiles.size() == 1 && profiles.front() == "No profile";
    const std::size_t profileCount = placeholder ? 0 : profiles.size();
    if (profileCount >= 64) {
        showMessage(language.get(
            "menu", "MAXPROFILES",
            "Spillerlisten er full. Fjern spillernavn som ikke er i bruk, og prøv igjen."));
        return;
    }
    pendingProfileName.clear();
    pendingBirthMonth = 0;
    pendingBirthDay = 1;
    dialogMode = DialogMode::Name;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StartTextInput();
    SDL_SetWindowTitle(window, "FGP Native — Fyll inn navn");
}

void AppCore::acceptNameDialog() {
    const auto cp1252 = utf8ToCp1252(pendingProfileName);
    if (!cp1252 || cp1252->empty() || cp1252->size() > 15) {
        return;
    }
    const auto duplicate = std::find(profiles.begin(), profiles.end(),
                                     pendingProfileName);
    const fs::path profilePath =
        profileDirectory / (pendingProfileName + ".pfl");
    if (duplicate != profiles.end() || fs::exists(profilePath)) {
        std::string message = language.get(
            "menu", "NAMEUSED",
            "Spillernavnet \"%s\" er allerede registrert.");
        const auto placeholder = message.find("%s");
        if (placeholder != std::string::npos) {
            message.replace(placeholder, 2, *cp1252);
        }
        showMessage(std::move(message));
        return;
    }
    dialogMode = DialogMode::Calendar;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StopTextInput();
    SDL_SetWindowTitle(window, "FGP Native — Fyll inn fødselsdato");
}

void AppCore::acceptCalendarDialog() {
    const auto cp1252 = utf8ToCp1252(pendingProfileName);
    if (!cp1252) {
        showMessage("Spillernavnet kan ikke lagres i spillets tegnsett.");
        return;
    }
    try {
        writeFreshProfile(profileDirectory, pendingProfileName, *cp1252,
                          pendingBirthMonth, pendingBirthDay);
    } catch (const std::exception &error) {
        showMessage(std::string("Kunne ikke opprette spilleren: ") + error.what());
        return;
    }

    if (profiles.size() == 1 && profiles.front() == "No profile") {
        profiles.clear();
    }
    profiles.push_back(pendingProfileName);
    std::sort(profiles.begin(), profiles.end());
    profileIndex = static_cast<std::size_t>(std::distance(
        profiles.begin(),
        std::find(profiles.begin(), profiles.end(), pendingProfileName)));
    dialogMode = DialogMode::None;
    dialogPressedControl = DialogControl::None;
    dialogPressedDay = 0;
    SDL_StopTextInput();
    setMenuTitle("Ny spiller opprettet");

    const std::time_t now = std::time(nullptr);
    const std::tm local = *std::localtime(&now);
    const int year = local.tm_year + 1900;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const bool birthday =
        local.tm_mon == pendingBirthMonth &&
        (local.tm_mday == pendingBirthDay ||
         (!leap && pendingBirthMonth == 1 && pendingBirthDay == 29 &&
          local.tm_mday == 28));
    if (birthday) {
        enterScene("birthday", "Gratulerer med dagen!");
    }
}

// Hotspot navigation recovered from the original's scene dispatchers, in
// src/generated/scene_navigations.hpp. Each scene module switches on the
// hotspot id and calls GAME_GotoSceneByName 0x0040cd70(name, entryMode) with
// both arguments as call-site constants, so the destination and the mode it is
// entered with are recoverable rather than guessed.
//
// town is handled here rather than in the table because its destination is not
// a constant: the scene caches its own entry mode at 0x005b8a0c (written at
// 0x00426fe2 from the initializer's first argument) and tests it at 0x0042701f,
// sending hotspots 27 and 32 to buildacar when it is 2 and to outtent
// otherwise. The same value chooses between the `ben` and `bike` objects.
bool AppCore::navigateGeneratedHotspot(std::uint32_t hotspot) {
    if (viewMode != ViewMode::Scene) {
        return false;
    }

    if (activeScene == "town" && (hotspot == 27U || hotspot == 32U)) {
        const bool builtCar = activeSceneEntryMode == 2;
        const char *target = builtCar ? "buildacar" : "outtent";
        std::cout << "Recovered town hotspot " << hotspot << " -> " << target
                  << " (entry mode " << activeSceneEntryMode
                  << ", 0x0042701f)\n";
        enterScene(target, target);
        return true;
    }

    for (const auto &definition : fgp_generated::kSceneNavigations) {
        if (definition.scene != activeScene || definition.hotspot != hotspot) {
            continue;
        }
        const std::string target(definition.target);
        std::cout << "Recovered " << activeScene << " hotspot " << hotspot
                  << " -> " << target << " (entry mode "
                  << definition.entryMode << ", from "
                  << std::hex << "0x" << definition.callSite << std::dec
                  << ")\n";
        enterScene(target, target, definition.entryMode);
        return true;
    }
    return false;
}

// The shared fact page: 0x0041dd40 scans the scene's fact.ini sections in
// declaration order for a matching `hotspot=`, and 0x00421310 presents the
// section name and joined paragraphs in the ordinary one-button message box.
// The section's ordinal is the bit index of the per-scene viewed mask, which
// 0x0041dbf0 reads from and 0x0041dc70 writes back to the profile record
// (current scene, type 0x402, key 100).
bool AppCore::showFactPage(std::uint32_t hotspot) {
    if (viewMode != ViewMode::Scene || sceneFacts.empty()) {
        return false;
    }
    const FactTable::Entry *entry = sceneFacts.findByHotspot(hotspot);
    if (!entry) {
        return false;
    }

    showMessage(FactTable::body(*entry), MessageKind::Fact);
    dialogTitle = entry->title;

    if (entry->ordinal >= 32) {
        return true;
    }
    const std::uint32_t bit = 1U << entry->ordinal;
    if ((sceneFactsViewed & bit) != 0U) {
        return true;
    }
    sceneFactsViewed |= bit;
    // The original defers this write to EVENT_EndScene; recording it as soon
    // as the page opens keeps the same value while using the existing atomic
    // profile writer.
    if (profiles[profileIndex] != "No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex] + ".pfl");
        if (fs::exists(profilePath)) {
            try {
                auto bytes = readBinaryFile(profilePath);
                upsertProfileSceneRecordU32(bytes, activeScene, 0x402U, 100U,
                                            sceneFactsViewed,
                                            ProfileRecordUpdate::BitwiseOr);
                writeProfileBytesAtomically(profilePath, bytes);
                const std::uint32_t completeMask = sceneFacts.completeMask();
                if (completeMask != 0U &&
                    (sceneFactsViewed & completeMask) == completeMask) {
                    pendingFactCompletionAward =
                        awardCarPart(profilePath, activeScene);
                }
            } catch (const std::exception &error) {
                std::cout << "Could not record viewed fact: " << error.what()
                          << '\n';
            }
        }
    }
    return true;
}

std::vector<std::string> AppCore::messageLines() {
    return wrapText(messageFont, dialogMessage, 330);
}

int AppCore::messageTextHeight() {
    return static_cast<int>(messageLines().size()) * messageFont.height;
}

int AppCore::messageDialogHeight() {
    // FGP.exe replaces the 443 px scalable middle of the 386x564
    // template with measuredTextHeight + 12. The fixed top and bottom
    // slices contribute 58 + 63 pixels.
    return 133 + messageTextHeight();
}

int AppCore::calendarDayAt(int x, int y) {
    static constexpr std::array<int, 12> daysInMonth{
        31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int localX = x - 250;
    const int localY = y - 157;
    if (localX < 13 || localX >= 286 || localY < 81 || localY >= 273) {
        return 0;
    }
    const int column = (localX - 13) / 39;
    const int row = (localY - 81) / 38;
    const int insideX = (localX - 13) % 39;
    const int insideY = (localY - 81) % 38;
    const int day = row * 7 + column + 1;
    return column >= 0 && column < 7 && row >= 0 && row < 5 &&
                   insideX < 36 && insideY < 34 &&
                   day <= daysInMonth[pendingBirthMonth]
               ? day
               : 0;
}

AppCore::DialogControl GameApp::dialogControlAt(int x, int y) {
    if (dialogMode == DialogMode::Name) {
        const int localX = x - 249;
        const int localY = y - 247;
        if (localX >= 15 && localX < 99 && localY >= 69 && localY < 99) {
            return DialogControl::NameOk;
        }
        if (localX >= 204 && localX < 288 && localY >= 69 && localY < 98) {
            return DialogControl::NameCancel;
        }
    } else if (dialogMode == DialogMode::Calendar) {
        const int localX = x - 250;
        const int localY = y - 157;
        if (localX >= 11 && localX < 52 && localY >= 40 && localY < 77) {
            return DialogControl::CalendarPrevious;
        }
        if (localX >= 249 && localX < 293 && localY >= 38 && localY < 78) {
            return DialogControl::CalendarNext;
        }
        if (localX >= 194 && localX < 281 && localY >= 239 && localY < 273) {
            return DialogControl::CalendarOk;
        }
        if (calendarDayAt(x, y) != 0) {
            return DialogControl::CalendarDay;
        }
    } else if (dialogMode == DialogMode::Message) {
        constexpr int dialogX = (800 - 380) / 2;
        const int textHeight = messageTextHeight();
        // 0x00421070: 300 - height / 2, halving each term.
        const int dialogY = 600 / 2 - messageDialogHeight() / 2;
        const int buttonY = dialogY + 85 + textHeight;
        const bool twoButtons =
            dialogMessageKind == MessageKind::ExitConfirmation ||
            dialogMessageKind == MessageKind::MorningRideConfirmation ||
            dialogMessageKind ==
                MessageKind::BuildacarDemolishConfirmation ||
            dialogMessageKind == MessageKind::ChessAbortConfirmation ||
            dialogMessageKind == MessageKind::RacingQuitConfirmation ||
            dialogMessageKind == MessageKind::ColorfillConfirmation ||
            dialogMessageKind == MessageKind::DraughtsAbortConfirmation ||
            dialogMessageKind ==
                MessageKind::ActivitypageClearConfirmation ||
            dialogMessageKind ==
                MessageKind::ActivitypagePrintConfirmation ||
            dialogMessageKind == MessageKind::TournamentAbortConfirmation;
        if (twoButtons &&
            x >= dialogX + 188 && x < dialogX + 271 &&
            y >= buttonY && y < buttonY + 27) {
            return DialogControl::MessagePrimary;
        }
        if (x >= dialogX + 275 && x < dialogX + 358 &&
            y >= buttonY && y < buttonY + 27) {
            return twoButtons
                       ? DialogControl::MessageSecondary
                       : DialogControl::MessagePrimary;
        }
    } else if (dialogMode == DialogMode::Award) {
        constexpr int dialogX = (800 - 317) / 2;
        constexpr int dialogY = (600 - 345) / 2;
        if (x >= dialogX + 216 && x < dialogX + 299 &&
            y >= dialogY + 297 && y < dialogY + 324) {
            return DialogControl::AwardClose;
        }
    } else if (dialogMode == DialogMode::HighScore) {
        constexpr int dialogX = (800 - 325) / 2;
        constexpr int dialogY = (600 - 395) / 2;
        const int localX = x - dialogX;
        const int localY = y - dialogY;
        if (localY >= 0 && localY < 23) {
            if (localX >= 14 && localX < 108) {
                return DialogControl::HighScoreProfile;
            }
            if (localX >= 112 && localX < 207) {
                return DialogControl::HighScoreGlobal;
            }
            if (localX >= 211 && localX < 304) {
                return DialogControl::HighScoreUnion;
            }
        }
        if (localX >= 221 && localX < 303 &&
            localY >= 352 && localY < 383) {
            return DialogControl::HighScoreClose;
        }
    } else if (dialogMode == DialogMode::Help) {
        constexpr int dialogX = 158;
        constexpr int dialogY = 93;
        const int localX = x - dialogX;
        const int localY = y - dialogY;
        if (localX >= 383 && localX < 466 &&
            localY >= 365 && localY < 392) {
            return DialogControl::HelpOk;
        }
        if (localX >= 10 && localX < 41 &&
            localY >= 364 && localY < 394) {
            return DialogControl::HelpPrevious;
        }
        if (localX >= 109 && localX < 140 &&
            localY >= 364 && localY < 394) {
            return DialogControl::HelpNext;
        }
        if (localX >= 450 && localX < 464 &&
            localY >= 51 && localY < 68) {
            return DialogControl::HelpScrollUp;
        }
        if (localX >= 450 && localX < 464 &&
            localY >= 338 && localY < 355) {
            return DialogControl::HelpScrollDown;
        }
        if (localX >= 450 && localX < 464 &&
            localY >= 82 && localY < 324) {
            return DialogControl::HelpScrollTrack;
        }
        if (helpTopicAt(x, y)) {
            return DialogControl::HelpTopic;
        }
    }
    return DialogControl::None;
}
} // namespace fgp
