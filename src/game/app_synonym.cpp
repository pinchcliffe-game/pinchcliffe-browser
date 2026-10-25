#include "game/app_synonym.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "assets/profile.h"
#include "game/synonym_rules.h"
#include "media/smacker.h"
#include "util/binary.h"
#include "util/text.h"

namespace fgp {
namespace {

constexpr SDL_Rect kStart{281,114,128,128};
constexpr SDL_Rect kHint{38,395,112,36};
constexpr SDL_Rect kPass{546,397,116,36};
constexpr SDL_Rect kShuffle{94,222,72,32};
constexpr SDL_Rect kSourceArea{188,228,321,32}; // 0x004A0520
constexpr SDL_Rect kTargetArea{110,276,433,90}; // 0x004A0530
constexpr std::size_t kPlacedDragBase = 1000U;
constexpr double kLetterReleaseSeconds = 0.133;
constexpr double kLetterTravelSpeed = 300.0;
constexpr double kLetterSpawnX = 518.0;

int centeredRowX(const SDL_Rect &area,int count,int itemSize) {
    return area.x+(area.w-count*itemSize)/2;
}

TexturePtr synonymClipTexture(SDL_Renderer *renderer,
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

double audioDuration(const DecodedMedia &media) {
    if (media.audioRate<=0 || media.audioChannels<=0) return 0.0;
    return static_cast<double>(media.audioSamples.size()) /
           static_cast<double>(media.audioRate*media.audioChannels);
}

TexturePtr keyedTexture(SDL_Renderer *renderer,
                        std::span<const std::uint8_t> bytes) {
    SurfacePtr surface = loadBmp(bytes);
    const Uint32 key = pixelValue(surface.get(),0,0);
    if (SDL_SetColorKey(surface.get(),SDL_TRUE,key) != 0)
        throw std::runtime_error(SDL_GetError());
    TexturePtr texture = makeTexture(renderer,surface.get());
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
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trim(line.substr(0,equals));
        std::string value = trim(line.substr(equals+1));
        values[std::move(key)] = std::move(value);
    }
    return values;
}

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(delimiter,begin);
        result.emplace_back(text.substr(begin,end == std::string_view::npos
                                                 ? text.size()-begin
                                                 : end-begin));
        if (end == std::string_view::npos) break;
        begin = end+1;
    }
    return result;
}

int letterIndex(unsigned char character) {
    if (character >= 'a' && character <= 'z') return character-'a';
    if (character == 0xe6) return 26;
    if (character == 0xf8) return 27;
    if (character == 0xe5) return 28;
    return -1;
}

} // namespace

SynonymScene::SynonymGame SynonymScene::loadSynonymGame(Package &package) {
    SynonymGame game;
    const auto asset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {sceneLanguagePackage.get(),
                 sceneAddonPackage.get(),sceneData2Package.get(),
                 sceneEditionPackage.get(),&package}) {
            if (!candidate) continue;
            if (const auto *entry = candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Synonym asset not found: "+std::string(name));
    };
    const auto commonAsset = [&](std::string_view name)
        -> std::span<const std::uint8_t> {
        for (Package *candidate : {addonCommon.get(),data2Common.get(),
                 editionCommon.get(),&common}) {
            if (!candidate) continue;
            if (const auto *entry=candidate->findOptional(name))
                return candidate->data(*entry);
        }
        throw std::runtime_error("Common asset not found: "+std::string(name));
    };
    // 0x00482960 passes `Arial_23_b` to the common-font loader at 0x00401000.
    // The exact 36-pixel strip is supplied by Data3/common.bin.
    game.promptFont=loadBitmapFont(
        renderer,commonAsset("fonts\\Arial_23_b.bmp"));
    game.startTexture = keyedTexture(renderer,asset("bitmap\\start.bmp"));
    game.hintTexture = keyedTexture(renderer,asset("bitmap\\hint.bmp"));
    game.passTexture = keyedTexture(renderer,asset("bitmap\\pass.bmp"));
    game.overlayLeftTexture = keyedTexture(renderer,asset("bitmap\\overlayleft.bmp"));
    game.overlayRightTexture = keyedTexture(renderer,asset("bitmap\\overlayright.bmp"));
    game.overlayFunnelTexture = keyedTexture(renderer,asset("bitmap\\overlayfunnel.bmp"));
    for (int state=0;state<3;++state) {
        game.hintButtonTextures[static_cast<std::size_t>(state)] = keyedTexture(
            renderer,asset("bitmap\\hint_"+std::to_string(state)+".bmp"));
        game.passButtonTextures[static_cast<std::size_t>(state)] = keyedTexture(
            renderer,asset("bitmap\\pass_"+std::to_string(state)+".bmp"));
    }
    for (int state=0;state<2;++state)
        game.shuffleTextures[static_cast<std::size_t>(state)] = keyedTexture(
            renderer,asset("bitmap\\shuffle_"+std::to_string(state)+".bmp"));
    std::array<std::string,29> names;
    for (std::size_t index=0;index<26;++index)
        names[index]=std::string(1,static_cast<char>('a'+index));
    names[26]=std::string(1,static_cast<char>(0xe6));
    names[27]=std::string(1,static_cast<char>(0xf8));
    names[28]=std::string(1,static_cast<char>(0xe5));
    for (std::size_t index=0;index<names.size();++index) {
        game.largeLetters[index] = keyedTexture(renderer,asset(
            "bitmap\\large_"+std::string(names[index])+".bmp"));
        game.smallLetters[index] = keyedTexture(renderer,asset(
            "bitmap\\small_"+std::string(names[index])+".bmp"));
    }
    const auto clipFrom=[&](std::string name,
                            std::span<const std::uint8_t> bytes,
                            int x,int y) {
        SynonymClip result;
        result.name=std::move(name); result.x=x; result.y=y;
        result.media=decodeSmacker(bytes,true,true,true,0,nullptr,
                                   SmackerIndexZeroAlpha::Transparent,
                                   activeSceneClipKeying(result.name));
        result.texture=synonymClipTexture(renderer,result.media);
        return result;
    };
    const auto clip=[&](std::string name,int x,int y) {
        const auto bytes=asset("animation\\"+name+".smk");
        return clipFrom(std::move(name),bytes,x,y);
    };
    // Descriptor coordinates are fields +0x54/+0x58 in the 0xA8-byte
    // records at 0x00514390, 0x00514438, 0x00513DA8 and 0x00513D00.
    game.ballStart1=clip("ballstart1",106,103);
    game.ballStart2=clip("ballstart2",106,103);
    game.time=clip("time",160,362);
    game.clock=clip("clock",241,110);
    game.correct=clip("correct",434,361);
    game.chain=clip("chain",139,339);
    game.clockring=clip("clockring",195,97);
    game.blank1=clipFrom("..\\..\\common\\animation\\blank1",
        common.data(common.find("animation\\blank1.smk")),0,0);
    for (std::size_t index=0;index<game.ballFallSounds.size();++index)
        game.ballFallSounds[index]=decodeSmacker(asset(
            "animation\\sndballfall"+std::to_string(index+1U)+".smk"),
            false,true,true);
    game.startBallSound=decodeSmacker(
        asset("animation\\sndstartball.smk"),false,true,true);
    game.clockTickSound=decodeSmacker(
        asset("animation\\sndclocktick.smk"),false,true,true);
    game.correctSound = decodeSmacker(activeSceneAnimationData("sndcorrect"),false,true,true);
    game.wrongSound = decodeSmacker(activeSceneAnimationData("sndwrong"),false,true,true);
    game.clickSound = decodeSmacker(activeSceneAnimationData("sndclick"),false,true,true);
    game.noHintSound = decodeSmacker(activeSceneAnimationData("sndnohint"),false,true,true);

    const auto dictionary = iniValues(asset("synonym.ini"));
    for (int index=0;;++index) {
        const auto word = dictionary.find("word"+std::to_string(index));
        if (word == dictionary.end()) break;
        const auto synonyms = dictionary.find("synonym"+std::to_string(index));
        if (synonyms == dictionary.end()) continue;
        const auto difficulty = dictionary.find("level"+std::to_string(index));
        game.entries.push_back({
            word->second,
            split(synonyms->second,':'),
            difficulty == dictionary.end() ? 0 : std::stoi(difficulty->second),
            static_cast<std::uint32_t>(game.entries.size()),
            0U,
        });
    }
    if (!profiles.empty() && profiles[profileIndex]!="No profile") {
        const fs::path profilePath =
            profileDirectory / (profiles[profileIndex]+".pfl");
        if (const auto ranks=profileSceneRecordBytes(
                profilePath,"synonym",0x402U,0U,
                game.entries.size()*sizeof(std::uint32_t))) {
            for (std::size_t index=0;index<game.entries.size();++index)
                game.entries[index].lastSeen=readLe32(*ranks,index*4U);
        }
    }
    const auto levels = iniValues(asset("level.ini"));
    const auto integer = [&](std::string key,int fallback) {
        const auto found=levels.find(key);
        return found == levels.end() ? fallback : std::stoi(found->second);
    };
    for (int level=0;level<12;++level) {
        auto &record=game.levels[static_cast<std::size_t>(level)];
        for (int difficulty=0;difficulty<3;++difficulty) {
            record.minimumLength[static_cast<std::size_t>(difficulty)] = integer(
                std::to_string(level)+"_minlength_"+std::to_string(difficulty),3);
            record.maximumLength[static_cast<std::size_t>(difficulty)] = integer(
                std::to_string(level)+"_maxlength_"+std::to_string(difficulty),4);
        }
        record.required=integer(std::to_string(level)+"_numcorrect",10);
        record.passDelay=integer(std::to_string(level)+"_wordtime",10000)/1000.0;
        record.hints=integer(std::to_string(level)+"_hintcount",1);
        record.totalSeconds=integer(std::to_string(level)+"_totaltime",180000)/1000.0;
        record.basePoints=integer(std::to_string(level)+"_basepoints",20);
        record.secondsPoints=integer(std::to_string(level)+"_secpoints",3);
    }
    std::cout << "Loaded INI-derived Synonym: " << game.entries.size()
              << " dictionary prompts, twelve exact level rows, original "
                 "letter balls, Hint/Shuffle/Pass and scoring clocks\n";
    return game;
}

void SynonymScene::saveSynonymRanks() {
    if (!synonymGame || profiles.empty() ||
        profiles[profileIndex]=="No profile") return;
    const fs::path profilePath =
        profileDirectory / (profiles[profileIndex]+".pfl");
    if (!fs::exists(profilePath)) return;
    std::vector<std::uint8_t> ranks(synonymGame->entries.size()*4U,0U);
    for (const auto &entry:synonymGame->entries)
        writeLe32(ranks,static_cast<std::size_t>(entry.stableId)*4U,
                  entry.lastSeen);
    try {
        // 0x00483128-0x00483170 gathers the stable dictionary ordinals and
        // writes the complete questionCount*4 record through key zero.
        setProfileSceneRecordBytes(
            profilePath,"synonym",0x402U,0U,ranks);
    } catch (const std::exception &error) {
        std::cerr << "Could not save Synonym ranks: " << error.what() << '\n';
    }
}

void SynonymScene::startSynonymGame() {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    game.showAttractResult=false;
    game.level=std::clamp(game.startingLevel,0,11);
    game.levelCorrect=0; game.score=0; game.totalHintsUsed=0;
    if (!prepareSynonymLevel()) return;
    beginSynonymRound();
}

bool SynonymScene::prepareSynonymLevel() {
    if (!synonymGame) return false;
    auto &game=*synonymGame;
    const auto &level=game.levels[static_cast<std::size_t>(game.level)];
    std::vector<SynonymRankCandidate> candidates;
    candidates.reserve(game.entries.size());
    for (std::size_t index=0;index<game.entries.size();++index) {
        const auto &entry=game.entries[index];
        const int length=static_cast<int>(entry.word.size());
        if (!synonymEntryEligible(entry.difficulty,length,
                                  level.minimumLength.data(),
                                  level.maximumLength.data())) continue;
        candidates.push_back({index,entry.difficulty,length,
                              entry.lastSeen,entry.stableId});
    }
    game.questionPool=orderSynonymCandidates(
        std::move(candidates),[&](int bound) {
            std::uniform_int_distribution<int> random(0,bound-1);
            return random(randomEngine);
        });
    if (game.questionPool.empty()) {
        game.phase=SynonymPhase::GameOver;
        game.phaseMessage="Ingen ord for dette niv\xE5" "et";
        return false;
    }
    game.currentQuestion=0U;
    game.levelStarted=sceneElapsedNow();
    game.levelDeadline=game.levelStarted+level.totalSeconds;
    game.clockFrame=synonymProgressFrame(
        game.clock.media.videoFrames.size(),1.0);
    return true;
}

void SynonymScene::beginSynonymRound() {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    if (game.questionPool.empty()) {
        game.phase=SynonymPhase::GameOver;
        return;
    }
    game.currentQuestion%=game.questionPool.size();
    auto &selected=game.entries[game.questionPool[game.currentQuestion]];
    game.prompt=selected.synonyms.empty()?std::string{}:selected.synonyms.front();
    game.answer=selected.word;
    selected.lastSeen=synonymMinuteStamp(std::time(nullptr));
    game.shuffledLetters.assign(game.answer.begin(),game.answer.end());
    do { std::shuffle(game.shuffledLetters.begin(),game.shuffledLetters.end(),randomEngine); }
    while (game.shuffledLetters.size()>1 && std::equal(
        game.shuffledLetters.begin(),game.shuffledLetters.end(),game.answer.begin()));
    game.placedLetters.clear(); game.draggingLetter.reset(); game.hintsUsed=0;
    game.transitionBalls.clear();
    const auto &level=game.levels[static_cast<std::size_t>(game.level)];
    game.roundPoints=synonymWordScore(
        level.basePoints,static_cast<int>(game.answer.size()),0);
    game.phase=SynonymPhase::LaunchBall1;
    game.launchedLetters=0U;
    game.draggingLetter.reset();
    game.ballStart1Started=sceneElapsedNow();
    game.ballStart2Started=-1.0;
    game.blankStarted=-1.0;
    game.chainStarted=-1.0;
    game.chainAudioNext=-1.0;
    game.clockringStarted=-1.0;
    referenceTrace.emit("animation_start",{
        {"asset",game.ballStart1.name},{"x",game.ballStart1.x},
        {"y",game.ballStart1.y},{"layer",10},{"loop",false}});
    referenceTrace.emit("animation_add",{
        {"asset",game.ballStart1.name},{"x",game.ballStart1.x},
        {"y",game.ballStart1.y},{"layer",10},{"conflict_mask",0}});
    audioMixer.addEffect(
        game.ballStart1.media,audioVolumes[0]*sceneAudioGains[0]);
    SDL_PauseAudioDevice(audioDevice,musicEnabled && !paused ? 0 : 1);
}

bool SynonymScene::beginSynonymInteraction(int x,int y) {
    if (!synonymGame || activeScene!="synonym") return false;
    auto &game=*synonymGame; const SDL_Point point{x,y};
    if (game.phase==SynonymPhase::Attract && SDL_PointInRect(&point,&kStart)) {
        startSynonymGame(); return true;
    }
    if (game.phase!=SynonymPhase::Playing) return false;
    if (SDL_PointInRect(&point,&kShuffle)) {
        do {
            std::shuffle(game.shuffledLetters.begin(),game.shuffledLetters.end(),
                         randomEngine);
        } while (game.shuffledLetters.size()>1U && game.placedLetters.empty() &&
                 std::equal(game.shuffledLetters.begin(),
                            game.shuffledLetters.end(),game.answer.begin()));
        audioMixer.setEffect(game.clickSound,audioVolumes[0]*sceneAudioGains[0]); return true;
    }
    if (SDL_PointInRect(&point,&kHint)) {
        const auto &level=game.levels[static_cast<std::size_t>(game.level)];
        if (game.hintsUsed>=level.hints || game.answer.size()<2) {
            audioMixer.setEffect(game.noHintSound,audioVolumes[0]*sceneAudioGains[0]);
        } else {
            const std::size_t answerIndex=(game.hintsUsed&1)==0
                ? static_cast<std::size_t>(game.hintsUsed/2)
                : game.answer.size()-1U-static_cast<std::size_t>(game.hintsUsed/2);
            const unsigned char wanted=static_cast<unsigned char>(game.answer[answerIndex]);
            auto found=std::find(game.shuffledLetters.begin(),game.shuffledLetters.end(),wanted);
            if (found!=game.shuffledLetters.end()) {
                game.placedLetters.insert(game.placedLetters.begin()+
                    std::min(answerIndex,game.placedLetters.size()),*found);
                game.shuffledLetters.erase(found);
            }
            ++game.hintsUsed;
            game.roundPoints=synonymWordScore(
                level.basePoints,static_cast<int>(game.answer.size()),
                game.hintsUsed);
        }
        return true;
    }
    if (SDL_PointInRect(&point,&kPass) &&
        sceneElapsedNow()-game.wordStarted>=
            game.levels[static_cast<std::size_t>(game.level)].passDelay) {
        beginSynonymTransition(true); return true;
    }
    const int sourceX=centeredRowX(
        kSourceArea,static_cast<int>(game.shuffledLetters.size()),32);
    for (std::size_t index=0;index<game.shuffledLetters.size();++index) {
        SDL_Rect bounds{sourceX+static_cast<int>(index)*32,kSourceArea.y,32,32};
        if (SDL_PointInRect(&point,&bounds)) {
            game.draggingLetter=index; game.dragX=x; game.dragY=y; return true;
        }
    }
    const int targetX=centeredRowX(
        kTargetArea,static_cast<int>(game.placedLetters.size()),40);
    for (std::size_t index=0;index<game.placedLetters.size();++index) {
        SDL_Rect bounds{targetX+static_cast<int>(index)*40,
                        kTargetArea.y+(kTargetArea.h-40)/2,40,40};
        if (SDL_PointInRect(&point,&bounds)) {
            game.draggingLetter=kPlacedDragBase+index; game.dragX=x; game.dragY=y; return true;
        }
    }
    return false;
}

void SynonymScene::moveSynonymInteraction(int x,int y) {
    if (synonymGame && synonymGame->draggingLetter) {
        synonymGame->dragX=x; synonymGame->dragY=y;
    }
}

void SynonymScene::endSynonymInteraction(int x,int y) {
    if (!synonymGame || !synonymGame->draggingLetter) return;
    auto &game=*synonymGame; const std::size_t encoded=*game.draggingLetter;
    unsigned char letter=0;
    if (encoded>=kPlacedDragBase) {
        const std::size_t index=encoded-kPlacedDragBase;
        if (index>=game.placedLetters.size()) { game.draggingLetter.reset(); return; }
        letter=game.placedLetters[index]; game.placedLetters.erase(game.placedLetters.begin()+static_cast<std::ptrdiff_t>(index));
    } else {
        if (encoded>=game.shuffledLetters.size()) { game.draggingLetter.reset(); return; }
        letter=game.shuffledLetters[encoded]; game.shuffledLetters.erase(game.shuffledLetters.begin()+static_cast<std::ptrdiff_t>(encoded));
    }
    if (y>=kTargetArea.y && y<=kTargetArea.y+kTargetArea.h) {
        const std::size_t insert=std::min(game.placedLetters.size(),
            static_cast<std::size_t>(std::max(0,x-centeredRowX(
                kTargetArea,static_cast<int>(game.placedLetters.size()+1U),40))/40));
        game.placedLetters.insert(game.placedLetters.begin()+static_cast<std::ptrdiff_t>(insert),letter);
    } else game.shuffledLetters.push_back(letter);
    game.draggingLetter.reset();
    if (game.placedLetters.size()==game.answer.size() &&
        std::equal(game.placedLetters.begin(),game.placedLetters.end(),game.answer.begin()))
        completeSynonymRound();
    else if (game.placedLetters.size()==game.answer.size())
        audioMixer.setEffect(game.wrongSound,audioVolumes[0]*sceneAudioGains[0]);
}

void SynonymScene::completeSynonymRound() {
    auto &game=*synonymGame;
    const auto &level=game.levels[static_cast<std::size_t>(game.level)];
    game.roundPoints=synonymWordScore(
        level.basePoints,static_cast<int>(game.answer.size()),game.hintsUsed);
    game.score+=game.roundPoints;
    game.totalHintsUsed+=game.hintsUsed;
    audioMixer.setEffect(game.correctSound,audioVolumes[0]*sceneAudioGains[0]);
    ++game.levelCorrect;
    beginSynonymTransition(false);
}

void SynonymScene::beginSynonymTransition(bool pass) {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    if (pass) game.totalHintsUsed+=game.hintsUsed;

    const std::size_t convertedStart=game.placedLetters.size();
    for (std::size_t index=0;index<game.shuffledLetters.size();++index) {
        std::string small="small_";
        small.push_back(static_cast<char>(game.shuffledLetters[index]));
        referenceTrace.emit("animation_stop",{
            {"asset",small},{"reason","converted"}});
        referenceTrace.emit("animation_remove",{
            {"asset",small},{"reason","converted"}});
        game.placedLetters.push_back(game.shuffledLetters[index]);
    }
    game.shuffledLetters.clear();
    game.transitionBalls.clear();
    const int x=centeredRowX(
        kTargetArea,static_cast<int>(game.placedLetters.size()),40);
    const double y=kTargetArea.y+(kTargetArea.h-40)/2.0;
    for (std::size_t index=0;index<game.placedLetters.size();++index) {
        game.transitionBalls.push_back({
            game.placedLetters[index],
            static_cast<double>(x+static_cast<int>(index)*40),y,false});
        if (index>=convertedStart) {
            std::string large="large_";
            large.push_back(static_cast<char>(game.placedLetters[index]));
            referenceTrace.emit("animation_start",{
                {"asset",large},{"x",x+static_cast<int>(index)*40},
                {"y",static_cast<int>(y)},{"layer",0},{"loop",false}});
            referenceTrace.emit("animation_add",{
                {"asset",large},{"x",x+static_cast<int>(index)*40},
                {"y",static_cast<int>(y)},{"layer",0},
                {"conflict_mask",0}});
        }
    }
    game.placedLetters.clear();
    game.draggingLetter.reset();
    game.phase=SynonymPhase::TransitionBlank;
    game.blankStarted=sceneElapsedNow();
    referenceTrace.emit("animation_start",{
        {"asset",game.blank1.name},{"x",game.blank1.x},
        {"y",game.blank1.y},{"layer",0},{"loop",false}});
    referenceTrace.emit("animation_add",{
        {"asset",game.blank1.name},{"x",game.blank1.x},
        {"y",game.blank1.y},{"layer",0},{"conflict_mask",0}});
}

void SynonymScene::advanceSynonymAfterTransition() {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    const auto &level=game.levels[static_cast<std::size_t>(game.level)];
    game.transitionBalls.clear();
    game.blankStarted=-1.0;
    game.chainStarted=-1.0;
    game.chainAudioNext=-1.0;
    if (game.levelCorrect>=level.required) {
        const int elapsedMilliseconds=static_cast<int>(
            std::max(0.0,sceneElapsedNow()-game.levelStarted)*1000.0);
        game.score+=synonymLevelBonus(
            elapsedMilliseconds,game.totalHintsUsed,level.secondsPoints);
        game.levelCorrect=0;
        if (game.level<11) ++game.level;
        game.totalHintsUsed=0;
        if (!prepareSynonymLevel()) return;
        beginSynonymRound();
        return;
    }
    game.currentQuestion=(game.currentQuestion+1U)%game.questionPool.size();
    beginSynonymRound();
}

void SynonymScene::skipSynonymPresentation() {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    if (game.phase==SynonymPhase::TransitionBlank ||
        game.phase==SynonymPhase::TransitionChain)
        advanceSynonymAfterTransition();
    if (game.phase==SynonymPhase::LaunchBall1 ||
        game.phase==SynonymPhase::LaunchBall2) {
        game.ballStart1Started=-1.0;
        game.ballStart2Started=-1.0;
        game.launchedLetters=game.shuffledLetters.size();
        game.phase=SynonymPhase::Playing;
        game.wordStarted=sceneElapsedNow();
    }
}

void SynonymScene::updateSynonym(double elapsed) {
    if (!synonymGame) return;
    auto &game=*synonymGame;
    const auto finishAnimation=[&](std::string_view name) {
        referenceTrace.emit("animation_stop",{
            {"asset",std::string(name)},{"reason","completed"}});
        referenceTrace.emit("animation_remove",{
            {"asset",std::string(name)},{"reason","removed"}});
    };
    const auto finishSound=[&](double &started,const DecodedMedia &media) {
        if (started<0.0 || elapsed-started<audioDuration(media)) return;
        // Original audio-only descriptors produce Stop/Remove bookkeeping but
        // no AnimationFinished boundary. Playback-lifecycle traces therefore
        // contain their start only, and native mirrors that public boundary.
        started=-1.0;
    };

    finishSound(game.startBallSoundStarted,game.startBallSound);
    finishSound(game.clockTickStarted,game.clockTickSound);

    if (game.phase!=SynonymPhase::Attract &&
        game.phase!=SynonymPhase::GameOver) {
        const auto &level=game.levels[static_cast<std::size_t>(game.level)];
        const double fraction=std::clamp(
            (game.levelDeadline-elapsed)/level.totalSeconds,0.0,1.0);
        const std::size_t frame=synonymProgressFrame(
            game.clock.media.videoFrames.size(),fraction);
        if (frame!=game.clockFrame) {
            game.clockFrame=frame;
            game.clockTickStarted=elapsed;
            referenceTrace.emit("animation_start",{
                {"asset","sndclocktick"},{"x",0},{"y",0},
                {"layer",0},{"loop",false}});
            referenceTrace.emit("animation_add",{
                {"asset","sndclocktick"},{"x",0},{"y",0},
                {"layer",0},{"conflict_mask",0}});
            audioMixer.addEffect(
                game.clockTickSound,audioVolumes[0]*sceneAudioGains[0]);
            SDL_PauseAudioDevice(
                audioDevice,musicEnabled && !paused ? 0 : 1);
        }
    }

    if (game.ballStart1Started>=0.0 &&
        elapsed-game.ballStart1Started>=game.ballStart1.media.videoDuration) {
        finishAnimation(game.ballStart1.name);
        game.ballStart1Started=-1.0;
        game.phase=SynonymPhase::LaunchBall2;
        game.launchStarted=elapsed;
        game.ballStart2Started=elapsed;
        referenceTrace.emit("animation_start",{
            {"asset",game.ballStart2.name},{"x",game.ballStart2.x},
            {"y",game.ballStart2.y},{"layer",10},{"loop",false}});
        referenceTrace.emit("animation_add",{
            {"asset",game.ballStart2.name},{"x",game.ballStart2.x},
            {"y",game.ballStart2.y},{"layer",10},{"conflict_mask",0}});
    }
    if (game.ballStart2Started>=0.0 &&
        elapsed-game.ballStart2Started>=game.ballStart2.media.videoDuration) {
        finishAnimation(game.ballStart2.name);
        game.ballStart2Started=-1.0;
    }

    if (game.phase==SynonymPhase::LaunchBall2) {
        const double launchElapsed=std::max(0.0,elapsed-game.launchStarted);
        const auto visible=synonymLaunchedLetterCount(
            static_cast<int>(std::floor(launchElapsed*1000.0)),
            game.shuffledLetters.size());
        while (game.launchedLetters<visible) {
            const std::size_t letter=game.launchedLetters++;
            std::string assetName="small_";
            assetName.push_back(static_cast<char>(game.shuffledLetters[letter]));
            referenceTrace.emit("animation_start",{
                {"asset",assetName},{"x",0},{"y",0},{"layer",0},
                {"hotspot",100+static_cast<int>(letter)},{"loop",false}});
            referenceTrace.emit("animation_add",{
                {"asset",assetName},{"x",0},{"y",0},{"layer",0},
                {"hotspot",100+static_cast<int>(letter)},
                {"conflict_mask",0}});
        }
        bool landed=game.launchedLetters==game.shuffledLetters.size();
        const int targetX=centeredRowX(
            kSourceArea,static_cast<int>(game.shuffledLetters.size()),32);
        for (std::size_t index=0;index<game.launchedLetters;++index) {
            const double age=std::max(
                0.0,launchElapsed-index*kLetterReleaseSeconds);
            const double x=std::max(
                static_cast<double>(targetX+static_cast<int>(index)*32),
                kLetterSpawnX-kLetterTravelSpeed*age);
            landed=landed &&
                x<=targetX+static_cast<int>(index)*32+0.5;
        }
        if (landed) {
            game.phase=SynonymPhase::Playing;
            game.wordStarted=elapsed;
            game.startBallSoundStarted=elapsed;
            referenceTrace.emit("animation_start",{
                {"asset","sndstartball"},{"x",0},{"y",0},
                {"layer",0},{"loop",false}});
            referenceTrace.emit("animation_add",{
                {"asset","sndstartball"},{"x",0},{"y",0},
                {"layer",0},{"conflict_mask",0}});
            audioMixer.addEffect(
                game.startBallSound,audioVolumes[0]*sceneAudioGains[0]);
            SDL_PauseAudioDevice(
                audioDevice,musicEnabled && !paused ? 0 : 1);
        }
    }

    if (game.phase==SynonymPhase::TransitionBlank &&
        elapsed-game.blankStarted>=game.blank1.media.videoDuration) {
        finishAnimation(game.blank1.name);
        game.blankStarted=-1.0;
        game.phase=SynonymPhase::TransitionChain;
        game.chainStarted=elapsed;
        const double chainAudioDuration=audioDuration(game.chain.media);
        game.chainAudioNext=chainAudioDuration>0.0
            ? elapsed+chainAudioDuration : -1.0;
        referenceTrace.emit("animation_start",{
            {"asset",game.chain.name},{"x",game.chain.x},
            {"y",game.chain.y},{"layer",10},{"loop",true}});
        referenceTrace.emit("animation_add",{
            {"asset",game.chain.name},{"x",game.chain.x},
            {"y",game.chain.y},{"layer",10},{"conflict_mask",0}});
        audioMixer.addEffect(
            game.chain.media,audioVolumes[0]*sceneAudioGains[0]);
        SDL_PauseAudioDevice(
            audioDevice,musicEnabled && !paused ? 0 : 1);
    }

    if (game.phase==SynonymPhase::TransitionChain) {
        const double chainElapsed=std::max(0.0,elapsed-game.chainStarted);
        const double chainAudioDuration=audioDuration(game.chain.media);
        if (game.chainAudioNext>=0.0 && chainAudioDuration>0.0) {
            while (elapsed>=game.chainAudioNext) {
                audioMixer.addEffect(
                    game.chain.media,audioVolumes[0]*sceneAudioGains[0]);
                game.chainAudioNext+=chainAudioDuration;
            }
        }
        bool allRemoved=true;
        for (auto &ball : game.transitionBalls) {
            if (ball.removed) continue;
            const auto position=synonymTransitionPosition(
                ball.startX,ball.startY,chainElapsed);
            if (position.visible) {
                allRemoved=false;
                continue;
            }
            ball.removed=true;
            std::uniform_int_distribution<int> sound(0,1);
            const std::size_t soundIndex=static_cast<std::size_t>(
                sound(randomEngine));
            const std::string soundName=
                "sndballfall"+std::to_string(soundIndex+1U);
            referenceTrace.emit("animation_start",{
                {"asset",soundName},{"x",0},{"y",0},
                {"layer",0},{"loop",false}});
            referenceTrace.emit("animation_add",{
                {"asset",soundName},{"x",0},{"y",0},
                {"layer",0},{"conflict_mask",0}});
            audioMixer.addEffect(game.ballFallSounds[soundIndex],
                                 audioVolumes[0]*sceneAudioGains[0]);
            std::string large="large_";
            large.push_back(static_cast<char>(ball.letter));
            referenceTrace.emit("animation_remove",{
                {"asset",large},{"reason","removed"}});
            referenceTrace.emit("animation_stop",{
                {"asset",large},{"reason","stopped"}});
        }
        if (allRemoved) {
            // The looping chain is explicitly removed by 0x00484000 rather
            // than reaching AnimationFinished. The trailing SCENE_StopAnim
            // is remove bookkeeping which the playback normalizer suppresses,
            // so native exposes only the same public removal boundary.
            referenceTrace.emit("animation_remove",{
                {"asset",game.chain.name},{"reason","removed"}});
            advanceSynonymAfterTransition();
        }
    }

    if (game.phase==SynonymPhase::Playing && elapsed>game.levelDeadline) {
        game.phase=SynonymPhase::TimeoutClockring;
        game.clockringStarted=elapsed;
        referenceTrace.emit("animation_start",{
            {"asset",game.clockring.name},{"x",game.clockring.x},
            {"y",game.clockring.y},{"layer",10},{"loop",false}});
        referenceTrace.emit("animation_add",{
            {"asset",game.clockring.name},{"x",game.clockring.x},
            {"y",game.clockring.y},{"layer",10},{"conflict_mask",0}});
        audioMixer.addEffect(
            game.clockring.media,audioVolumes[0]*sceneAudioGains[0]);
        SDL_PauseAudioDevice(
            audioDevice,musicEnabled && !paused ? 0 : 1);
    }
    if (game.phase==SynonymPhase::TimeoutClockring &&
        elapsed-game.clockringStarted>=game.clockring.media.videoDuration) {
        finishAnimation(game.clockring.name);
        game.clockringStarted=-1.0;
        game.transitionBalls.clear();
        game.phase=SynonymPhase::GameOver;
        showHighScore(game.score);
    }
}

void SynonymScene::renderSynonym(double elapsed) {
    if (!synonymGame) return; auto &game=*synonymGame;
    const auto renderClipFrame=[&](SynonymClip &clip,std::size_t frameIndex) {
        if (!clip.texture || clip.media.videoFrames.empty()) return;
        frameIndex=std::min(frameIndex,clip.media.videoFrames.size()-1U);
        const auto &frame=clip.media.videoFrames[frameIndex];
        uploadVideoFrame(clip.texture.get(),frame);
        SDL_Rect destination{clip.x,clip.y,frame.width,frame.height};
        SDL_RenderCopy(renderer,clip.texture.get(),nullptr,&destination);
    };
    const auto renderPlayingClip=[&](SynonymClip &clip,double started) {
        if (started<0.0 || clip.media.videoFrames.empty()) return;
        renderClipFrame(clip,frameIndexAt(
            clip.media,std::max(0.0,elapsed-started),false));
    };
    if (game.phase!=SynonymPhase::Attract) {
        const auto &level=game.levels[static_cast<std::size_t>(game.level)];
        const double fraction=std::clamp(
            (game.levelDeadline-elapsed)/level.totalSeconds,0.0,1.0);
        renderClipFrame(game.time,synonymProgressFrame(
            game.time.media.videoFrames.size(),fraction));
        renderClipFrame(game.clock,synonymProgressFrame(
            game.clock.media.videoFrames.size(),fraction));
        const double correctFraction=level.required>0
            ? static_cast<double>(game.levelCorrect)/level.required : 0.0;
        renderClipFrame(game.correct,synonymProgressFrame(
            game.correct.media.videoFrames.size(),correctFraction));
        renderPlayingClip(game.ballStart1,game.ballStart1Started);
        renderPlayingClip(game.ballStart2,game.ballStart2Started);
        renderPlayingClip(game.blank1,game.blankStarted);
        if (game.chainStarted>=0.0 && !game.chain.media.videoFrames.empty())
            renderClipFrame(game.chain,frameIndexAt(
                game.chain.media,
                std::max(0.0,elapsed-game.chainStarted),true));
        renderPlayingClip(game.clockring,game.clockringStarted);
    }
    SDL_Rect left{145,224,48,48},right{505,224,48,48},funnel{547,318,128,84};
    SDL_RenderCopy(renderer,game.overlayLeftTexture.get(),nullptr,&left);
    SDL_RenderCopy(renderer,game.overlayRightTexture.get(),nullptr,&right);
    SDL_RenderCopy(renderer,game.overlayFunnelTexture.get(),nullptr,&funnel);
    SDL_Rect hintLabel{11,431,124,20},passLabel{559,432,120,20};
    SDL_RenderCopy(renderer,game.hintTexture.get(),nullptr,&hintLabel);
    SDL_RenderCopy(renderer,game.passTexture.get(),nullptr,&passLabel);
    SDL_RenderCopy(renderer,game.shuffleTextures[0].get(),nullptr,&kShuffle);
    drawCenteredText(renderer,messageFont,
        language.get("synonym","POINTS","Poeng"),SDL_Rect{295,372,93,12});
    drawText(renderer,messageFont,
        language.get("synonym","LEVEL","Niv\xE5"),295,417);
    if (game.phase==SynonymPhase::Attract) {
        SDL_RenderCopy(renderer,game.hintButtonTextures[0].get(),nullptr,&kHint);
        SDL_RenderCopy(renderer,game.passButtonTextures[0].get(),nullptr,&kPass);
        SDL_RenderCopy(renderer,game.startTexture.get(),nullptr,&kStart);
        if (!game.showAttractResult) return;
    } else {
        const auto &level=game.levels[static_cast<std::size_t>(game.level)];
        const std::size_t hintState=game.phase==SynonymPhase::Playing
            ? (game.hintsUsed>=level.hints?2U:0U) : 2U;
        const std::size_t passState=game.phase==SynonymPhase::Playing &&
            elapsed-game.wordStarted>=level.passDelay ? 0U : 2U;
        SDL_RenderCopy(renderer,game.hintButtonTextures[hintState].get(),nullptr,&kHint);
        SDL_RenderCopy(renderer,game.passButtonTextures[passState].get(),nullptr,&kPass);
        if (game.phase!=SynonymPhase::LaunchBall1) {
            SDL_SetTextureColorMod(game.promptFont.texture.get(),255,0,0);
            drawCenteredText(
                renderer,game.promptFont,game.prompt,SDL_Rect{187,166,321,37});
            SDL_SetTextureColorMod(game.promptFont.texture.get(),255,255,255);
        }
    }
    const int sourceX=centeredRowX(
        kSourceArea,static_cast<int>(game.shuffledLetters.size()),32);
    const std::size_t visibleSource=game.phase==SynonymPhase::LaunchBall1
        ? 0U : game.phase==SynonymPhase::LaunchBall2
            ? game.launchedLetters : game.shuffledLetters.size();
    const double launchElapsed=std::max(0.0,elapsed-game.launchStarted);
    for (std::size_t i=0;i<visibleSource;++i) {
        if (game.draggingLetter && *game.draggingLetter==i) continue;
        const int index=letterIndex(game.shuffledLetters[i]); if (index<0) continue;
        const int settledX=sourceX+static_cast<int>(i)*32;
        const double age=std::max(
            0.0,launchElapsed-i*kLetterReleaseSeconds);
        const int x=game.phase==SynonymPhase::LaunchBall2
            ? static_cast<int>(std::lround(std::max(
                static_cast<double>(settledX),
                kLetterSpawnX-kLetterTravelSpeed*age)))
            : settledX;
        SDL_Rect dst{x,kSourceArea.y,32,32};
        SDL_RenderCopy(renderer,game.smallLetters[static_cast<std::size_t>(index)].get(),nullptr,&dst);
    }
    const int targetX=centeredRowX(
        kTargetArea,static_cast<int>(game.placedLetters.size()),40);
    const bool showPlacedLetters=game.phase==SynonymPhase::Playing ||
        game.phase==SynonymPhase::TimeoutClockring ||
        game.phase==SynonymPhase::GameOver ||
        (game.phase==SynonymPhase::Attract && game.showAttractResult);
    const bool interactiveLetters=game.phase==SynonymPhase::Playing;
    for (std::size_t i=0;showPlacedLetters && i<game.placedLetters.size();++i) {
        if (game.draggingLetter && *game.draggingLetter==kPlacedDragBase+i) continue;
        const int index=letterIndex(game.placedLetters[i]); if (index<0) continue;
        SDL_Rect dst{targetX+static_cast<int>(i)*40,
                     kTargetArea.y+(kTargetArea.h-40)/2,40,40};
        SDL_RenderCopy(renderer,game.largeLetters[static_cast<std::size_t>(index)].get(),nullptr,&dst);
    }
    const double chainElapsed=std::max(0.0,elapsed-game.chainStarted);
    for (const auto &ball : game.transitionBalls) {
        if (ball.removed) continue;
        const auto position=game.phase==SynonymPhase::TransitionChain
            ? synonymTransitionPosition(ball.startX,ball.startY,chainElapsed)
            : SynonymTransitionPosition{ball.startX,ball.startY,true};
        if (!position.visible) continue;
        const int index=letterIndex(ball.letter); if (index<0) continue;
        SDL_Rect dst{static_cast<int>(std::lround(position.x)),
                     static_cast<int>(std::lround(position.y)),40,40};
        SDL_RenderCopy(renderer,
            game.largeLetters[static_cast<std::size_t>(index)].get(),
            nullptr,&dst);
    }
    if (interactiveLetters && game.draggingLetter) {
        unsigned char value=0;
        const bool sourceLetter=*game.draggingLetter<kPlacedDragBase;
        const std::size_t i=sourceLetter
            ?*game.draggingLetter:*game.draggingLetter-kPlacedDragBase;
        if (sourceLetter && i<game.shuffledLetters.size())
            value=game.shuffledLetters[i];
        if (!sourceLetter && i<game.placedLetters.size())
            value=game.placedLetters[i];
        const int index=letterIndex(value); if (index>=0) {
            const int size=sourceLetter?32:40;
            SDL_Rect dst{game.dragX-size/2,game.dragY-size/2,size,size};
            SDL_RenderCopy(renderer,
                (sourceLetter?game.smallLetters:game.largeLetters)
                    [static_cast<std::size_t>(index)].get(),nullptr,&dst);
        }
    }
    drawCenteredText(renderer,messageFont,std::to_string(game.score),SDL_Rect{295,384,93,33});
    const std::string levelText=std::to_string(game.level+1);
    drawText(renderer,messageFont,levelText,
             388-textWidth(messageFont,levelText),417);
}

std::optional<std::pair<std::uint32_t,SDL_Rect>> SynonymScene::synonymHotspotAt(int x,int y) const {
    if (!synonymGame || activeScene!="synonym") return std::nullopt; const SDL_Point p{x,y};
    if (synonymGame->phase==SynonymPhase::Attract && SDL_PointInRect(&p,&kStart)) return std::pair{1U,kStart};
    if (synonymGame->phase==SynonymPhase::Playing) {
        if (SDL_PointInRect(&p,&kHint)) return std::pair{401U,kHint};
        if (SDL_PointInRect(&p,&kPass)) return std::pair{402U,kPass};
        if (SDL_PointInRect(&p,&kShuffle)) return std::pair{403U,kShuffle};
    }
    return std::nullopt;
}

bool SynonymScene::executeSynonymDeveloperCommand(std::string_view command) {
    constexpr std::string_view synonymControlPrefix = "synonym:";
    if (activeScene!="synonym" || !synonymGame) return false;
    const auto control=command.substr(synonymControlPrefix.size());
    const auto ensurePlaying=[&]() {
        if (synonymGame->phase==SynonymPhase::Attract ||
            synonymGame->phase==SynonymPhase::TimeoutClockring ||
            synonymGame->phase==SynonymPhase::GameOver)
            startSynonymGame();
        // Developer rule controls intentionally bypass both recovered
        // presentation sequences. Physical/reference input retains them.
        skipSynonymPresentation();
    };
    if (control=="start") startSynonymGame();
    else if (control=="difficulty:easy" || control=="difficulty:medium" || control=="difficulty:hard")
        synonymGame->startingLevel=control=="difficulty:easy"?0:
            control=="difficulty:medium"?2:4;
    else if (control=="solve") {
        ensurePlaying();
        synonymGame->placedLetters.assign(synonymGame->answer.begin(),synonymGame->answer.end());
        synonymGame->shuffledLetters.clear(); completeSynonymRound();
        skipSynonymPresentation();
    } else if (control=="hint") {
        ensurePlaying();
        beginSynonymInteraction(50,410);
    } else if (control=="shuffle") {
        ensurePlaying();
        beginSynonymInteraction(110,235);
    } else if (control=="pass") {
        ensurePlaying();
        synonymGame->wordStarted-=20.0; beginSynonymInteraction(580,410);
        skipSynonymPresentation();
    } else if (control.rfind("level:",0)==0) {
        const int level=std::stoi(std::string(control.substr(6)));
        if (level<1 || level>12) return false;
        ensurePlaying();
        synonymGame->level=level-1; synonymGame->levelCorrect=0;
        synonymGame->totalHintsUsed=0;
        if (!prepareSynonymLevel()) return false;
        beginSynonymRound();
        skipSynonymPresentation();
    } else if (control.rfind("set-score=",0)==0) {
        // The score the timeout hands to 0x0041f9d0 (a capture writes it at
        // 0x00483ecf); retried until a game is running.
        if (synonymGame->phase==SynonymPhase::Attract) return false;
        synonymGame->score=std::stoi(std::string(control.substr(10)));
    } else if (control=="timeout") {
        ensurePlaying();
        synonymGame->levelDeadline=sceneElapsedNow()-1.0;
    } else if (control=="status") {
        std::cout << "Synonym status: level " << synonymGame->level+1
                  << ", correct " << synonymGame->levelCorrect << '/'
                  << synonymGame->levels[static_cast<std::size_t>(synonymGame->level)].required
                  << ", score " << synonymGame->score << ", prompt "
                  << synonymGame->prompt << ", answer " << synonymGame->answer
                  << ", placed " << synonymGame->placedLetters.size() << '\n';
    } else throw std::runtime_error("Unknown developer Synonym control: "+std::string(control));
    return true;
}

} // namespace fgp
