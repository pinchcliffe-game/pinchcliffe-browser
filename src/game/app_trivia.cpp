#include "game/app_trivia.h"
#include "assets/original_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "assets/profile.h"
#include "game/trivia_rules.h"
#include "media/smacker.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {
namespace {

constexpr SDL_Rect kStart{652,173,113,108};
constexpr SDL_Rect kQuestionDragSource{648,171,122,114};
constexpr SDL_Rect kQuestionReplay{711,299,63,57};
constexpr SDL_Point kQuestionDragSize{32,28};
constexpr std::array<SDL_Point,4> kFramePositions{{
    {0,25},{19,0},{0,54},{4,54}}};
constexpr std::array<SDL_Point,3> kSet3{{{49,122},{302,68},{259,227}}};
constexpr std::array<SDL_Point,4> kSet4{{
    {84,35},{284,64},{87,211},{285,217}}};
constexpr std::array<SDL_Point,5> kSet5{{
    {39,78},{224,99},{369,115},{151,238},{310,238}}};
constexpr std::array<SDL_Point,6> kSet6{{
    {47,87},{208,104},{338,120},{50,238},{210,239},{338,239}}};
constexpr std::array<SDL_Point,4> kChoiceSizes{{
    {168,144},{152,140},{144,124},{124,108}}};
constexpr std::array<SDL_Point,4> kFrameSizes{{
    {436,356},{412,396},{468,316},{424,312}}};

enum TriviaAnimationIndex : std::size_t {
    Correct,
    Wrong,
    Miss,
    Timeout,
    NextPicture1,
    NextPicture2,
    NextPicture3,
    NextPicture4,
    Shutter,
};
struct TriviaAnimationSpec {
    std::string_view name;
    int x{};
    int y{};
    std::uint32_t conflictMask{};
    bool visible{};
};
const auto kTriviaAnimationSpecs = originalArray<TriviaAnimationSpec, 9>([](const OriginalData &source, std::size_t index) {
    constexpr std::array<std::uint32_t, 9> descriptors{
        0x00511b30U, 0x00511bd8U, 0x00511c80U, 0x00511d28U, 0x00511278U, 0x00511320U, 0x005113c8U, 0x00511470U, 0x00511518U};
    const auto address = descriptors[index];
    return TriviaAnimationSpec{source.string(address + 0), source.read<int>(address + 0x54), source.read<int>(address + 0x58), source.read<std::uint32_t>(address + 0x60), source.read<int>(address + 0x44) != 0};
});

TexturePtr keyedTexture(SDL_Renderer *renderer,
                        std::span<const std::uint8_t> bytes) {
    SurfacePtr surface=loadBmp(bytes);
    const Uint32 green=SDL_MapRGB(surface->format,0,255,0);
    SDL_SetColorKey(surface.get(),SDL_TRUE,green);
    TexturePtr texture=makeTexture(renderer,surface.get());
    SDL_SetTextureBlendMode(texture.get(),SDL_BLENDMODE_BLEND);
    return texture;
}

TexturePtr triviaAnimationTexture(SDL_Renderer *renderer,
                                  const DecodedMedia &media) {
    if (media.videoFrames.empty()) return {};
    const auto &frame=media.videoFrames.front();
    TexturePtr texture(SDL_CreateTexture(
        renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,
        frame.width,frame.height));
    if (!texture)
        throw std::runtime_error(std::string("SDL_CreateTexture failed: ")+
                                 SDL_GetError());
    SDL_SetTextureBlendMode(texture.get(),SDL_BLENDMODE_BLEND);
    return texture;
}

std::unordered_map<std::string,std::string>
iniValues(std::span<const std::uint8_t> data) {
    std::string text(reinterpret_cast<const char *>(data.data()),data.size());
    std::istringstream lines(text);
    std::unordered_map<std::string,std::string> values;
    std::string line;
    while (std::getline(lines,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        const auto equals=line.find('=');
        if (equals==std::string::npos || (!line.empty() && line.front()=='[')) continue;
        values[trim(line.substr(0,equals))]=trim(line.substr(equals+1));
    }
    return values;
}

SDL_Point choicePosition(int count,int index) {
    if (count==3) return kSet3[static_cast<std::size_t>(index)];
    if (count==4) return kSet4[static_cast<std::size_t>(index)];
    if (count==5) return kSet5[static_cast<std::size_t>(index)];
    return kSet6[static_cast<std::size_t>(index)];
}

} // namespace

TriviaScene::TriviaGame TriviaScene::loadTriviaGame(Package &package) {
    TriviaGame game;
    const auto asset=[&](std::string_view name)->std::span<const std::uint8_t>{
        for (Package *candidate:{sceneLanguagePackage.get(),sceneAddonPackage.get(),
                 sceneData2Package.get(),sceneEditionPackage.get(),&package}) {
            if (!candidate) continue;
            if (const auto *entry=candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Trivia asset not found: "+std::string(name));
    };
    game.startTexture=keyedTexture(renderer,asset("bitmap\\start.bmp"));
    game.progressTexture=keyedTexture(renderer,asset("bitmap\\progress.bmp"));
    game.correctTexture=keyedTexture(renderer,asset("bitmap\\correct.bmp"));
    game.timeTexture=keyedTexture(renderer,asset("bitmap\\time.bmp"));
    game.radioTexture=keyedTexture(renderer,asset("bitmap\\radio.bmp"));
    game.dragTexture=keyedTexture(
        renderer,asset("bitmap\\mouse\\slide_cursor.bmp"));
    for (std::size_t index=0;index<game.animations.size();++index) {
        const auto &spec=kTriviaAnimationSpecs[index];
        auto &clipAsset=game.animations[index];
        clipAsset.name=spec.name;
        clipAsset.x=spec.x;
        clipAsset.y=spec.y;
        clipAsset.conflictMask=spec.conflictMask;
        clipAsset.visible=spec.visible;
        clipAsset.media=decodeSmacker(
            asset("animation\\"+clipAsset.name+".smk"),true,true,true,0,
            nullptr,SmackerIndexZeroAlpha::Transparent,
            activeSceneClipKeying(clipAsset.name));
        clipAsset.texture=triviaAnimationTexture(renderer,clipAsset.media);
    }

    // Pointer table 0x00511E78 fixes the stable profile-id order.
    for (const auto file:{"bitmapq.ini","soundq.ini","textq.ini"}) {
        const auto values=iniValues(asset(file));
        for (int index=0;;++index) {
            const std::string suffix=std::to_string(index);
            const auto type=values.find("type"+suffix);
            if (type==values.end()) break;
            TriviaQuestion question;
            question.type=std::stoi(type->second);
            question.difficulty=std::stoi(values.at("difficulty"+suffix));
            question.scene=std::stoi(values.at("scene"+suffix));
            question.stableId=static_cast<std::uint32_t>(game.questions.size());
            const char *key=question.type==0?"image":question.type==1?"text":"sound";
            question.payload=values.at(std::string(key)+suffix);
            if (question.payload.size()>=2 && question.payload.front()=='"' &&
                question.payload.back()=='"')
                question.payload=question.payload.substr(1,question.payload.size()-2);
            game.questions.push_back(std::move(question));
        }
    }
    if (!profiles.empty() && profiles[profileIndex]!="No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex]+".pfl");
        if (const auto ranks=profileSceneRecordBytes(
                profilePath,"gametrivia",0x402U,0U,
                game.questions.size()*sizeof(std::uint32_t))) {
            for (std::size_t index=0;index<game.questions.size();++index)
                game.questions[index].lastSeen=readLe32(*ranks,index*4U);
        }
    }
    const auto values=iniValues(asset("trivia.ini"));
    const auto integer=[&](std::string key,int fallback) {
        const auto found=values.find(key);
        return found==values.end()?fallback:std::stoi(found->second);
    };
    for (int round=0;round<4;++round) {
        auto &record=game.rounds[static_cast<std::size_t>(round)];
        record.questions=integer("questions"+std::to_string(round),8);
        record.scenes=integer("scenes"+std::to_string(round),3+round);
        record.basePoints=integer("basepoints"+std::to_string(round),100+round*50);
        record.secondsPoints=integer("secpoints"+std::to_string(round),40+round*30);
        for (int difficulty=0;difficulty<3;++difficulty) {
            record.difficulty[static_cast<std::size_t>(difficulty)]=integer(
                "difficulty"+std::to_string(round)+"_"+std::to_string(difficulty),difficulty);
            record.seconds[static_cast<std::size_t>(difficulty)]=integer(
                "time"+std::to_string(round)+"_"+std::to_string(difficulty),10000)/1000.0;
        }
    }
    for (int index=0;;++index) {
        const std::string suffix=std::to_string(index);
        const auto scene=values.find("scene"+suffix);
        const auto picture=values.find("name"+suffix);
        if (scene==values.end() || picture==values.end()) break;
        game.answers.push_back(TriviaAnswer{
            std::stoi(scene->second),std::stoi(picture->second)});
    }
    std::cout << "Loaded INI-derived Trivia: " << game.questions.size()
              << " image/text/sound questions, " << game.answers.size()
              << " scene-to-picture answers, exact four-round timing and "
                 "3–6 original layouts\n";
    return game;
}

void TriviaScene::saveTriviaRanks() {
    if (!triviaGame || profiles.empty() ||
        profiles[profileIndex]=="No profile") return;
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex]+".pfl");
    if (!fs::exists(profilePath)) return;
    std::vector<std::uint8_t> ranks(triviaGame->questions.size()*4U,0U);
    for (const auto &question:triviaGame->questions)
        writeLe32(ranks,static_cast<std::size_t>(question.stableId)*4U,
                  question.lastSeen);
    try {
        // 0x0047F7D0-0x0047F81C writes the whole stable-id array only when
        // the scene ends, through type 0x402/key 0.
        setProfileSceneRecordBytes(
            profilePath,"gametrivia",0x402U,0U,ranks);
    } catch (const std::exception &error) {
        std::cerr << "Could not save Trivia ranks: " << error.what() << '\n';
    }
}

void TriviaScene::startTriviaGame() {
    if (!triviaGame) return;
    auto &game=*triviaGame;
    game.phase=TriviaPhase::Playing; game.round=0; game.questionNumber=0;
    game.correctAnswers=0; game.score=0; game.phaseMessage.clear();
    game.draggingQuestion=false;
    if (!prepareTriviaRound()) return;
    beginTriviaQuestion();
}

void TriviaScene::playTriviaAnimation(std::size_t animationIndex,
                                  bool emitNamedStart) {
    if (!triviaGame || animationIndex>=triviaGame->animations.size()) return;
    auto &game=*triviaGame;
    const auto &clipAsset=game.animations[animationIndex];
    const double startedAt=sceneElapsedNow();
    // EVENT_StartAnimation at 0x0047F9F0 reports the selected transition
    // before its visible descriptor and shutter are added. Feedback clips are
    // created directly through 0x00412C20 and therefore only report an add.
    if (emitNamedStart)
        referenceTrace.emit("animation_start",{
            {"asset",clipAsset.name},{"x",clipAsset.x},{"y",clipAsset.y},
            {"layer",0},{"loop",false}});
    referenceTrace.emit("animation_add",{
        {"asset",clipAsset.name},{"x",clipAsset.x},{"y",clipAsset.y},
        {"layer",0},{"conflict_mask",clipAsset.conflictMask}});
    game.activeAnimations.push_back({animationIndex,startedAt});
    audioMixer.addEffect(clipAsset.media,audioVolumes[0]*sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice,musicEnabled && !paused ? 0 : 1);
}

void TriviaScene::startTriviaTransition(bool correct) {
    if (!triviaGame) return;
    auto &game=*triviaGame;
    const auto choice=triviaTransitionChoice(
        triviaTransitionSelector,correct,[this](int bound) {
            return particleRandom(bound);
        });
    const auto selected=NextPicture1+choice;
    const auto &clipAsset=game.animations[selected];
    const double requestedAt=sceneElapsedNow();

    // 0x0047FCAD submits the selected descriptor through the shared external
    // (category-5) scheduler at 0x0040A770. 0x0040A5D0 marks every matching
    // character group pending and 0x0040A610 refuses to add the descriptor
    // while that group's current clip is still running. Preserve the named
    // request now, but defer the visible add to the live Solan boundary.
    referenceTrace.emit("animation_start",{
        {"asset",clipAsset.name},{"x",clipAsset.x},{"y",clipAsset.y},
        {"layer",0},{"loop",false}});
    game.pendingTransitionAnimation=selected;
    game.pendingTransitionStartsAt=sceneCharacterConflictReleaseAt(
        clipAsset.conflictMask,requestedAt);
    sceneSequencePendingConflictMask|=clipAsset.conflictMask;
}

bool TriviaScene::prepareTriviaRound() {
    if (!triviaGame) return false;
    auto &game=*triviaGame;
    if (game.round<0 || game.round>=static_cast<int>(game.rounds.size())) {
        game.phase=TriviaPhase::GameOver;
        return false;
    }
    const auto &round=game.rounds[static_cast<std::size_t>(game.round)];
    const int wantedDifficulty=
        round.difficulty[static_cast<std::size_t>(game.difficulty)];

    // 0x0047EA50 -> 0x0047EC40 builds one answer-scene set for the
    // complete round, then limits its question pointer array to those scenes
    // and the configured difficulty. The original randomizes the eligible
    // answer and question orders before taking the requested counts.
    std::vector<TriviaAnswer> eligibleAnswers;
    for (const auto &answer:game.answers) {
        const bool hasQuestion=std::any_of(
            game.questions.begin(),game.questions.end(),[&](const auto &question) {
                return question.scene==answer.scene &&
                       question.difficulty==wantedDifficulty;
            });
        if (hasQuestion) eligibleAnswers.push_back(answer);
    }
    if (static_cast<int>(eligibleAnswers.size())<round.scenes) {
        game.phase=TriviaPhase::GameOver;
        game.phaseMessage="Ingen gyldig spørsmålsrunde";
        return false;
    }
    std::vector<TriviaAnswerRankCandidate> answerCandidates;
    answerCandidates.reserve(eligibleAnswers.size());
    for (std::size_t answerIndex=0; answerIndex<eligibleAnswers.size();
         ++answerIndex) {
        const auto &answer=eligibleAnswers[answerIndex];
        std::uint64_t timestampSum=0;
        std::size_t matchingQuestions=0;
        for (const auto &question:game.questions) {
            if (question.scene==answer.scene &&
                question.difficulty==wantedDifficulty) {
                timestampSum+=question.lastSeen;
                ++matchingQuestions;
            }
        }
        answerCandidates.push_back({answerIndex,
            static_cast<std::uint32_t>(timestampSum/matchingQuestions),
            static_cast<std::uint32_t>(answerIndex)});
    }
    const auto answerOrder=orderTriviaAnswers(
        std::move(answerCandidates),[this](int bound) {
            return particleRandom(bound);
        });
    std::vector<TriviaAnswer> orderedAnswers;
    orderedAnswers.reserve(answerOrder.size());
    for (const auto answerIndex:answerOrder)
        orderedAnswers.push_back(eligibleAnswers[answerIndex]);
    orderedAnswers.resize(static_cast<std::size_t>(round.scenes));
    eligibleAnswers=std::move(orderedAnswers);

    game.answerScenes.clear();
    game.answerPictures.clear();
    for (const auto &answer:eligibleAnswers) {
        game.answerScenes.push_back(answer.scene);
        game.answerPictures.push_back(answer.picture);
    }

    std::vector<TriviaRankCandidate> candidates;
    for (std::size_t index=0;index<game.questions.size();++index) {
        const auto &question=game.questions[index];
        if (question.difficulty!=wantedDifficulty) continue;
        if (std::find(game.answerScenes.begin(),game.answerScenes.end(),
                      question.scene)!=game.answerScenes.end())
            candidates.push_back({index,question.scene,question.difficulty,
                                  question.lastSeen,question.stableId});
    }
    game.roundQuestions=orderTriviaCandidates(
        std::move(candidates),[this](int bound) {
            return particleRandom(bound);
        });
    if (game.roundQuestions.size()<static_cast<std::size_t>(round.questions)) {
        game.phase=TriviaPhase::GameOver;
        game.phaseMessage="For få spørsmål i runden";
        return false;
    }
    game.roundQuestions.resize(static_cast<std::size_t>(round.questions));

    const auto asset=[&](std::string_view name)->std::span<const std::uint8_t>{
        for (Package *candidate:{sceneLanguagePackage.get(),sceneAddonPackage.get(),
                 sceneData2Package.get(),sceneEditionPackage.get(),scenePackage.get()}) {
            if (!candidate) continue;
            if (const auto *entry=candidate->findOptional(name)) return candidate->data(*entry);
        }
        throw std::runtime_error("Trivia round asset not found: "+std::string(name));
    };
    const int count=round.scenes;
    const int setIndex=count-3;
    game.answerFrameTexture=keyedTexture(renderer,asset(
        "bitmap\\set"+std::to_string(count)+"\\frame.bmp"));
    for (int index=0;index<count;++index) {
        const std::string prefix="bitmap\\set"+std::to_string(count)+"\\"+
            std::to_string(index+1)+"\\"+std::to_string(count)+"_pictures";
        std::ostringstream filename; filename<<prefix;
        filename.width(4); filename.fill('0');
        filename<<game.answerPictures[static_cast<std::size_t>(index)]<<".bmp";
        game.answerTextures[static_cast<std::size_t>(index)]=
            keyedTexture(renderer,asset(filename.str()));
        const SDL_Point pos=choicePosition(count,index);
        game.answerBounds[static_cast<std::size_t>(index)]={pos.x,pos.y,
            kChoiceSizes[static_cast<std::size_t>(setIndex)].x,
            kChoiceSizes[static_cast<std::size_t>(setIndex)].y};
    }
    for (int index=count;index<6;++index)
        game.answerTextures[static_cast<std::size_t>(index)].reset();
    return true;
}

void TriviaScene::endTriviaGame() {
    // 0x0047fa9e -> 0x0047fab0 after the last round. Outside a tournament
    // it formats gametrivia:GAMEOVER with the correct answers (0x006da520)
    // and the questions of every round (0x0047e5e0) under
    // interfaceh:GAMEOVER (0x0047fb36); then 0x0047fb4b passes the score
    // [0x006dba30] and the table 0x004a03d8 to 0x0041f9d0
    // (GameApp::closeDialog). In a tournament it goes straight there.
    auto &game=*triviaGame;
    game.phase=TriviaPhase::GameOver;
    game.phaseMessage.clear();
    if (tournament.active) {
        showHighScore(game.score);
        return;
    }
    int questions=0;
    for (const auto &round:game.rounds) questions+=round.questions;
    const std::string format=language.get(
        "gametrivia","GAMEOVER","Ikke dårlig! Du klarte %d av %d mulige.");
    char text[512]{};
    std::snprintf(text,sizeof text,format.c_str(),game.correctAnswers,questions);
    showMessageWithTitle(language.get("interfaceh","GAMEOVER","Spillet er slutt"),
                         text,MessageKind::TriviaGameOver);
}

void TriviaScene::returnTriviaToAttract() {
    // 0x0047fb5c-0x0047fb6a: `start` at layer 0x190 once the score closes.
    if (!triviaGame) return;
    triviaGame->phase=TriviaPhase::Attract;
    referenceTrace.emit("animation_add",{
        {"asset","start"},{"x",-1},{"y",-1},{"layer",400},{"hotspot",0},
        {"conflict_mask",0}});
}

void TriviaScene::beginTriviaQuestion() {
    if (!triviaGame) return; auto &game=*triviaGame;
    game.draggingQuestion=false;
    if (game.questionNumber>=game.rounds[static_cast<std::size_t>(game.round)].questions) {
        game.questionNumber=0;
        if (++game.round>=4) {
            endTriviaGame();
            return;
        }
        if (!prepareTriviaRound()) return;
    }
    const auto &round=game.rounds[static_cast<std::size_t>(game.round)];
    if (game.questionNumber<0 ||
        static_cast<std::size_t>(game.questionNumber)>=game.roundQuestions.size()) {
        game.phase=TriviaPhase::GameOver;
        return;
    }
    auto &question=game.questions[game.roundQuestions[
        static_cast<std::size_t>(game.questionNumber)]];
    question.lastSeen=triviaMinuteStamp(std::time(nullptr));
    game.currentScene=question.scene; game.questionText.clear();
    // 0x0047E760 removes the preceding question descriptor before creating the
    // next one. For sound questions that also ends the prior question audio;
    // otherwise a sound prompt could leak into a following text/image prompt.
    audioMixer.clearForeground();
    game.questionTexture.reset(); game.questionAudio={};

    const auto asset=[&](std::string_view name)->std::span<const std::uint8_t>{
        for (Package *candidate:{sceneLanguagePackage.get(),sceneAddonPackage.get(),
                 sceneData2Package.get(),sceneEditionPackage.get(),scenePackage.get()}) {
            if (!candidate) continue;
            if (const auto *entry=candidate->findOptional(name)) return candidate->data(*entry);
        }
        throw std::runtime_error("Trivia round asset not found: "+std::string(name));
    };
    if (question.type==0) {
        game.questionTexture=keyedTexture(renderer,asset(
            "bitmap\\image\\"+question.payload+".bmp"));
    } else if (question.type==1) {
        game.questionText=question.payload;
    } else {
        game.questionAudio=decodeSmacker(asset("sound\\"+question.payload+".wav"),false,true,true);
        audioMixer.setForeground(game.questionAudio,
            audioVolumes[1]*sceneAudioGains[1]);
    }

    game.deadline=sceneElapsedNow()+round.seconds[static_cast<std::size_t>(game.difficulty)];
    game.phase=TriviaPhase::Playing; game.phaseMessage.clear();
    const auto correct=std::find(
        game.answerScenes.begin(),game.answerScenes.end(),question.scene);
    if (correct!=game.answerScenes.end()) {
        const std::size_t slot=static_cast<std::size_t>(
            correct-game.answerScenes.begin());
        const auto &bounds=game.answerBounds[slot];
        const auto wrong=std::find_if(
            game.answerScenes.begin(),game.answerScenes.end(),
            [&](int scene) { return scene!=question.scene; });
        const std::size_t wrongSlot=static_cast<std::size_t>(
            wrong-game.answerScenes.begin());
        const auto &wrongBounds=game.answerBounds[wrongSlot];
        referenceTrace.emit("minigame_question",{
            {"game","trivia"},
            {"question",game.round*8+game.questionNumber},
            {"scene",question.scene},
            {"difficulty",question.difficulty},
            {"hotspot",100+question.scene},
            {"logical_x",bounds.x+bounds.w/2},
            {"logical_y",bounds.y+bounds.h/2},
            {"wrong_scene",*wrong},
            {"wrong_x",wrongBounds.x+wrongBounds.w/2},
            {"wrong_y",wrongBounds.y+wrongBounds.h/2},
            {"answer_count",static_cast<int>(game.answerScenes.size())},
        });
    }
}

void TriviaScene::answerTrivia(std::size_t answer) {
    if (!triviaGame || triviaGame->phase!=TriviaPhase::Playing ||
        answer>=triviaGame->answerScenes.size()) return;
    auto &game=*triviaGame;
    const int selectedScene=game.answerScenes[answer];
    const double answeredAt=sceneElapsedNow();
    const auto &round=game.rounds[static_cast<std::size_t>(game.round)];
    const int remaining=std::max(0,static_cast<int>(
        std::floor(game.deadline-answeredAt)));
    game.lastCorrect=selectedScene==game.currentScene;
    int pointsAwarded=0;
    if (game.lastCorrect) {
        playTriviaAnimation(Correct,false);
        // 0x0047FD99-0x0047FDB8 divides the positive millisecond remainder
        // by 1000 with signed integer truncation, then clamps negative time
        // to zero. That is floor, not ceil, in the native seconds clock.
        pointsAwarded=triviaCorrectScore(
            round.basePoints,round.secondsPoints,game.deadline-answeredAt);
        game.score+=pointsAwarded;
        ++game.correctAnswers; game.phaseMessage="Riktig!";
    } else {
        playTriviaAnimation(Wrong,false);
        game.phaseMessage="Feil bilde";
    }
    referenceTrace.emit("minigame_answer",{
        {"game","trivia"},
        {"result",game.lastCorrect?"correct":"wrong"},
        {"selected_scene",selectedScene},
        {"correct_scene",game.currentScene},
        {"remaining_seconds",remaining},
        {"points_awarded",pointsAwarded},
        {"score",game.score},
        {"correct_count",game.correctAnswers},
    });
    ++game.questionNumber; game.phase=TriviaPhase::Feedback;
    startTriviaTransition(game.lastCorrect);
}

bool TriviaScene::beginTriviaInteraction(int x,int y) {
    if (!triviaGame || activeScene!="gametrivia") return false;
    auto &game=*triviaGame; const SDL_Point point{x,y};
    if ((game.phase==TriviaPhase::Attract || game.phase==TriviaPhase::GameOver) &&
        SDL_PointInRect(&point,&kStart)) { startTriviaGame(); return true; }
    if (game.phase!=TriviaPhase::Playing) return false;
    // The original does not answer on a direct painting click. Hotspot 5
    // starts `mouse\\slide_cursor`; 0x0040C1F0 registers that descriptor and
    // 0x0040C260 hit-tests its release point before emitting event 0x10B.
    if (SDL_PointInRect(&point,&kQuestionDragSource)) {
        game.draggingQuestion=true;
        return true;
    }
    if (!game.questionAudio.audioSamples.empty()) {
        if (SDL_PointInRect(&point,&kQuestionReplay)) {
            audioMixer.setForeground(game.questionAudio,audioVolumes[1]*sceneAudioGains[1]);
            return true;
        }
    }
    return false;
}

bool TriviaScene::endTriviaInteraction(int x,int y) {
    if (!triviaGame || activeScene!="gametrivia" ||
        !triviaGame->draggingQuestion) return false;
    auto &game=*triviaGame;
    game.draggingQuestion=false;
    const SDL_Point point{x,y};
    for (std::size_t index=0;index<game.answerScenes.size();++index) {
        if (SDL_PointInRect(&point,&game.answerBounds[index])) {
            answerTrivia(index);
            break;
        }
    }
    // A release over no answer is the `miss` branch at 0x0047FD5B and leaves
    // the current question active. Either way the drag consumes the release.
    if (game.phase==TriviaPhase::Playing) playTriviaAnimation(Miss,false);
    return true;
}

void TriviaScene::updateTrivia(double elapsed) {
    if (!triviaGame) return;
    auto &game=*triviaGame;
    if (game.phase==TriviaPhase::Playing && elapsed>=game.deadline) {
        game.draggingQuestion=false;
        game.lastCorrect=false; game.phaseMessage="Tiden er ute";
        playTriviaAnimation(Timeout,false);
        ++game.questionNumber; game.phase=TriviaPhase::Feedback;
        startTriviaTransition(false);
    }

    if (game.pendingTransitionAnimation.has_value() &&
        elapsed>=game.pendingTransitionStartsAt) {
        const auto selected=*game.pendingTransitionAnimation;
        const auto conflictMask=game.animations[selected].conflictMask;
        // The retry path at 0x0040AB29-0x0040AB76 reaches 0x0040A660 only
        // after 0x0040A610 sees no running conflicting group descriptor.
        // Acquire the same live boundary, hold Solan during the transition,
        // and only now expose the next-picture descriptor to 0x0047F9F0's
        // shutter handler.
        acquireSceneCharacterConflicts(conflictMask,elapsed);
        sceneSequencePendingConflictMask&=~conflictMask;
        sceneSequenceHeldConflictMask|=conflictMask;
        game.pendingTransitionAnimation.reset();
        playTriviaAnimation(selected,false);
        playTriviaAnimation(Shutter,false);
    }

    bool transitionCompleted=false;
    std::uint32_t completedTransitionConflictMask=0U;
    for (auto active=game.activeAnimations.begin();
         active!=game.activeAnimations.end();) {
        const auto &clipAsset=game.animations[active->animation];
        if (elapsed-active->startedAt<
            originalSmackerCompletionDuration(clipAsset.media)) {
            ++active;
            continue;
        }
        referenceTrace.emit("animation_stop",{
            {"asset",clipAsset.name},{"reason","completed"}});
        referenceTrace.emit("animation_remove",{
            {"asset",clipAsset.name},{"reason","removed"}});
        const bool completedTransition=
            active->animation>=NextPicture1 &&
            active->animation<=NextPicture4;
        transitionCompleted=transitionCompleted || completedTransition;
        if (completedTransition)
            completedTransitionConflictMask|=clipAsset.conflictMask;
        active=game.activeAnimations.erase(active);
    }
    if (transitionCompleted && game.phase==TriviaPhase::Feedback) {
        sceneSequenceHeldConflictMask&=~completedTransitionConflictMask;
        beginTriviaQuestion();
    }
}

void TriviaScene::renderTrivia(double elapsed) {
    if (!triviaGame) return; auto &game=*triviaGame;
    if (game.phase==TriviaPhase::Attract || game.phase==TriviaPhase::GameOver) {
        // A finished game has no `start` until the score closes (0x0047fb6a).
        if (game.phase==TriviaPhase::Attract || !game.phaseMessage.empty())
            SDL_RenderCopy(renderer,game.startTexture.get(),nullptr,&kStart);
        if (game.phase==TriviaPhase::GameOver)
            drawCenteredText(renderer,messageFont,game.phaseMessage,SDL_Rect{172,665,520,26});
        return;
    }
    const int count=static_cast<int>(game.answerScenes.size());
    if (count>=3) {
        const int setIndex=count-3;
        SDL_Rect frame{kFramePositions[static_cast<std::size_t>(setIndex)].x,
                       kFramePositions[static_cast<std::size_t>(setIndex)].y,
                       kFrameSizes[static_cast<std::size_t>(setIndex)].x,
                       kFrameSizes[static_cast<std::size_t>(setIndex)].y};
        SDL_RenderCopy(renderer,game.answerFrameTexture.get(),nullptr,&frame);
        for (int index=0;index<count;++index)
            SDL_RenderCopy(renderer,game.answerTextures[static_cast<std::size_t>(index)].get(),
                           nullptr,&game.answerBounds[static_cast<std::size_t>(index)]);
    }
    const SDL_Rect screen{650,176,112,101};
    if (game.questionTexture)
        SDL_RenderCopy(renderer,game.questionTexture.get(),nullptr,&screen);
    else if (!game.questionText.empty()) {
        int y=180;
        for (const auto &line:wrapText(messageFont,game.questionText,106)) {
            drawCenteredText(renderer,messageFont,line,SDL_Rect{653,y,106,messageFont.height});
            y+=messageFont.height; if (y>265) break;
        }
    } else {
        SDL_Rect radio{727,330,12,12};
        SDL_RenderCopy(renderer,game.radioTexture.get(),nullptr,&radio);
        drawCenteredText(renderer,messageFont,"LYTT",SDL_Rect{697,292,75,28});
    }
    const int totalQuestions=std::accumulate(
        game.rounds.begin(),game.rounds.end(),0,
        [](int total,const TriviaRound &configured) {
            return total+configured.questions;
        });
    int questionsBeforeRound=0;
    for (int index=0;index<game.round;++index)
        questionsBeforeRound+=game.rounds[static_cast<std::size_t>(index)].questions;
    const int answeredQuestions=questionsBeforeRound+game.questionNumber;
    const int displayedQuestion=answeredQuestions+
        (game.phase==TriviaPhase::Playing ? 1 : 0);

    // The four labels and two counters are separate text controls in the
    // original HUD. The first fraction is correct/answered; the second is the
    // current question across all four rounds (1/32 on the first question),
    // rather than the native pass's former zero-based per-round 0/8 counter.
    drawCenteredText(renderer,messageFont,
        language.get("gametrivia","SOUND","Spill lyd"),SDL_Rect{7,517,129,27});
    drawCenteredText(renderer,messageFont,
        language.get("gametrivia","CORRECT","Riktige svar"),SDL_Rect{7,548,129,21});
    drawCenteredText(renderer,messageFont,
        language.get("gametrivia","PROGRESS","Status"),SDL_Rect{7,572,129,20});
    drawCenteredText(renderer,messageFont,
        language.get("gametrivia","SCORE","Poeng"),SDL_Rect{574,517,91,27});
    drawCenteredText(renderer,messageFont,
        std::to_string(game.correctAnswers)+"/"+std::to_string(answeredQuestions),
        SDL_Rect{507,548,61,21});
    drawCenteredText(renderer,messageFont,
        std::to_string(displayedQuestion)+"/"+std::to_string(totalQuestions),
        SDL_Rect{507,572,61,20});

    const auto &round=game.rounds[static_cast<std::size_t>(game.round)];
    const double total=round.seconds[static_cast<std::size_t>(game.difficulty)];
    const double fraction=std::clamp((game.deadline-elapsed)/total,0.0,1.0);
    const int correctWidth=totalQuestions>0
        ? 360*game.correctAnswers/totalQuestions : 0;
    const int progressWidth=totalQuestions>0
        ? 360*displayedQuestion/totalQuestions : 0;
    const int timeWidth=static_cast<int>(201*fraction);

    // These are fill strips, not full-width HUD backgrounds: the backdrop owns
    // the frames. 0x0047F72C/0x0047F736-0x0047F740 create `correct` at
    // (144,550), 0x0047F754/0x0047F75E-0x0047F763 create `progress` at
    // (144,571), and 0x0047F774/0x0047F77E-0x0047F783 create the 201x24 `time`
    // gradient at (580,558). Each is cropped from the right as its fraction
    // changes; the bitmap's key colour leaves the backdrop visible elsewhere.
    if (correctWidth>0) {
        SDL_Rect source{0,0,correctWidth,16};
        SDL_Rect destination{144,550,correctWidth,16};
        SDL_RenderCopy(renderer,game.correctTexture.get(),&source,&destination);
    }
    if (progressWidth>0) {
        SDL_Rect source{0,0,progressWidth,16};
        SDL_Rect destination{144,571,progressWidth,16};
        SDL_RenderCopy(renderer,game.progressTexture.get(),&source,&destination);
    }
    if (timeWidth>0) {
        SDL_Rect source{0,0,timeWidth,24};
        SDL_Rect destination{580,558,timeWidth,24};
        SDL_RenderCopy(renderer,game.timeTexture.get(),&source,&destination);
    }
    drawCenteredText(renderer,messageFont,std::to_string(game.score),SDL_Rect{700,517,90,27});
    if (game.draggingQuestion) {
        SDL_Rect drag{mouseX,mouseY,kQuestionDragSize.x,kQuestionDragSize.y};
        SDL_RenderCopy(renderer,game.dragTexture.get(),nullptr,&drag);
    }
    for (const auto &active:game.activeAnimations) {
        auto &clipAsset=game.animations[active.animation];
        if (!clipAsset.visible || !clipAsset.texture ||
            clipAsset.media.videoFrames.empty()) continue;
        const auto frameIndex=frameIndexAt(
            clipAsset.media,std::max(0.0,elapsed-active.startedAt),false);
        const auto &frame=clipAsset.media.videoFrames[frameIndex];
        uploadVideoFrame(clipAsset.texture.get(),frame);
        SDL_Rect destination{clipAsset.x,clipAsset.y,frame.width,frame.height};
        SDL_RenderCopy(renderer,clipAsset.texture.get(),nullptr,&destination);
    }
}

std::optional<std::pair<std::uint32_t,SDL_Rect>>
TriviaScene::triviaHotspotAt(int x,int y) const {
    if (!triviaGame || activeScene!="gametrivia") return std::nullopt;
    const SDL_Point point{x,y};
    if ((triviaGame->phase==TriviaPhase::Attract || triviaGame->phase==TriviaPhase::GameOver) &&
        SDL_PointInRect(&point,&kStart)) return std::pair{3U,kStart};
    if (triviaGame->phase==TriviaPhase::Playing &&
        SDL_PointInRect(&point,&kQuestionDragSource))
        return std::pair{5U,kQuestionDragSource};
    if (triviaGame->phase==TriviaPhase::Playing)
        for (std::size_t i=0;i<triviaGame->answerScenes.size();++i)
            if (SDL_PointInRect(&point,&triviaGame->answerBounds[i]))
                return std::pair{
                    100U+static_cast<std::uint32_t>(triviaGame->answerScenes[i]),
                    triviaGame->answerBounds[i]};
    if (triviaGame->phase==TriviaPhase::Playing &&
        SDL_PointInRect(&point,&kQuestionReplay))
        return std::pair{7U,kQuestionReplay};
    return std::nullopt;
}

bool TriviaScene::executeTriviaDeveloperCommand(std::string_view command) {
    constexpr std::string_view triviaControlPrefix = "trivia:";
    if (activeScene!="gametrivia" || !triviaGame) return false;
    const auto control=command.substr(triviaControlPrefix.size());
    if (control=="start") startTriviaGame();
    else if (control=="difficulty:easy" || control=="difficulty:medium" || control=="difficulty:hard")
        triviaGame->difficulty=control=="difficulty:easy"?0:control=="difficulty:medium"?1:2;
    else if (control=="miss") {
        if (triviaGame->phase!=TriviaPhase::Playing) startTriviaGame();
        triviaGame->draggingQuestion=true;
        endTriviaInteraction(799,599);
    }
    else if (control=="correct" || control=="wrong") {
        if (triviaGame->phase!=TriviaPhase::Playing) startTriviaGame();
        auto found=std::find(triviaGame->answerScenes.begin(),triviaGame->answerScenes.end(),triviaGame->currentScene);
        std::size_t answer=static_cast<std::size_t>(found-triviaGame->answerScenes.begin());
        if (control=="wrong") answer=(answer+1U)%triviaGame->answerScenes.size();
        answerTrivia(answer);
    } else if (control.rfind("round:",0)==0) {
        if (triviaGame->phase==TriviaPhase::Attract) startTriviaGame();
        const int round=std::stoi(std::string(control.substr(6)));
        if (round<1 || round>4) return false;
        triviaGame->round=round-1;
        triviaGame->questionNumber=0;
        if (prepareTriviaRound()) beginTriviaQuestion();
    } else if (control=="finish") {
        // Ends the game as the last round's last question does (0x0047fab0).
        if (triviaGame->phase!=TriviaPhase::Playing) return false;
        endTriviaGame();
    } else if (control=="timeout") {
        if (triviaGame->phase!=TriviaPhase::Playing) startTriviaGame();
        triviaGame->deadline=sceneElapsedNow()-1.0;
    } else if (control=="status") {
        std::cout << "Trivia status: round " << triviaGame->round+1
                  << ", question " << triviaGame->questionNumber+1
                  << ", score " << triviaGame->score << ", correct "
                  << triviaGame->correctAnswers << ", answer scene "
                  << triviaGame->currentScene << ", wall scenes ";
        for (std::size_t index=0;
             index<triviaGame->answerScenes.size();++index) {
            if (index!=0) std::cout << ',';
            std::cout << triviaGame->answerScenes[index];
        }
        std::cout << '\n';
    } else throw std::runtime_error("Unknown developer Trivia control: "+std::string(control));
    return true;
}

} // namespace fgp
