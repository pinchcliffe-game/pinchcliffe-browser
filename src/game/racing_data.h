#pragma once

// The race's data, as FGP.exe's loaders read it: the parameter block a race
// is described by, track.ini and car.ini, and the three binary side files.
// See "Racing" in REVERSE_ENGINEERING.md and src/generated/racing.json.
// Nothing here touches SDL or the scene, so it is unit-tested on its own.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "util/crt_rand.h"

namespace fgp::racing {

// 0x7fffffff: no best time (0x00474a42) and a car with no progress.
constexpr std::uint32_t kNoTime = 0x7fffffffU;

// The parameter block (0x006cfed0 from the racing menu's builder
// 0x0047c000, or the story block 0x0050aaf0).
struct RaceBlock {
    std::string track;                      // +0x00
    std::array<std::string, 4> cars;        // +0x20 .. +0x80
    int laps{3};                            // +0xa0
    int cars_count{4};                      // +0xa4
    int mode{};                             // +0xa8: 0 one player, 1 all AI, 2 split
    std::uint32_t best{kNoTime};            // +0xac, loaded at entry
    std::uint32_t tricks{};                 // +0xb0
    std::uint32_t raceTimeKey{};            // +0x17c (0x0047c100)
    std::uint32_t lapTimeKey{};             // +0x180 (0x0047c0e0)
    // The block pointer is 0x0050aaf0: the story race.
    bool story{};
};

// The story block at 0x0050aaf0, decoded into racing.json's story_race.
RaceBlock storyRaceBlock();

// 0x0047c120: slot 0 takes player 1's car and, when the mode is 1 or 2 with
// more than one car (0x0047ab30), slot 1 player 2's. The rest are drawn
// without repeats (0x004120e0, CRT rand) from a pool of race.ini cars 0..6
// less the chosen cars. carResourceName maps a race.ini resource number to
// its resource name (0x0047c260). car1Index and car2Index are the menu's
// list indices, which the original compares with the pool's resource
// numbers (0x0040a0f0 at 0x0047c1c8, 0x0047c1ed); a failed find still
// removes element -1, dropping the pool's first entry (0x0040a0b0).
void drawRaceCars(RaceBlock &block, int car1Resource, int car2Resource,
                  int car1Index, int car2Index,
                  const std::function<std::string(int)> &carResourceName,
                  CrtRandom &random);

// One [terrainN] entry, 16 bytes at track +0x7c (defaults 0x0050aa20).
struct TerrainEntry {
    float friction{1.0F};
    float pitch{1.0F};
    float volume{1.0F};
    float repair{0.0F};
};

struct TrackOverlay {
    int x{};
    int y{};
    std::string source;
    int ontop{};
};

// RACE_LoadTrack 0x00475270's reads of [track] and [overlays].
struct TrackSpec {
    int id{-1};                 // +0x00
    int award{-1};              // +0x04
    float difficulty{1.0F};     // +0x17c
    float speed{1.0F};          // +0x180
    std::string track;          // the track bitmap
    std::array<int, 4> startX{};
    std::array<int, 4> startY{};
    float startAngle{};         // +0x44, radians
    std::string terrain;
    std::array<TerrainEntry, 16> terrains{};
    std::string map;
    std::string sounds;         // may be empty
    std::string collision;
    std::string path;
    std::vector<TrackOverlay> overlays;
};

TrackSpec parseTrackIni(std::span<const std::uint8_t> bytes);

// RACE_LoadCar 0x00475ce0's reads of [car] and, for an AI car, [bias].
struct CarSpec {
    int id{1};                  // +0x00
    float accel{350.0F};        // +0x108
    float topSpeed{350.0F};     // +0x10c
    float width{};              // +0x110
    float height{};             // +0x114
    float braking{700.0F};      // +0x120
    float turning{};            // +0x124, radians per second
    float grip{0.8F};           // +0x128
    float mass{1.0F};           // +0x12c
    float shield{1.0F};         // +0x130
    int gears{4};               // +0x138
    float difficulty{1.0F};     // +0x298
    // +0x150..+0x178: normal, sneak, damaged, line 3 (666), then pickup,
    // car, wall, trick, track, horn.
    std::array<float, 4> lane{1.0F, 1.0F, 1.0F, 666.0F};
    float pickup{};
    float car{};
    float wall{};
    float trick{};
    float trackWeight{};
    float horn{};
    bool ai{};                  // +0x1f4
};

CarSpec parseCarIni(std::span<const std::uint8_t> bytes, bool ai);

// The loader opens "%s/stage%d.ini" instead of car.ini when the car path's
// last five characters are "tempo" (0x00475d5e-0x00475d91).
bool usesStageFile(const std::string &resource);

// 0x004750f0: 1 + max(0, track difficulty - the player's difficulty), the
// player being slot 0 in mode 0 and the mean of slots 0 and 1 in mode 2; mode
// 1 returns 1.1 (0x004a00f4) outright.
float loadScale(int mode, const TrackSpec &track,
                const std::vector<CarSpec> &cars);
// 0x00475170 (every AI car by the scale, grip by its cube) and 0x00475220
// (every car's accel and top speed by the track's speed).
void applyLoadScaling(std::vector<CarSpec> &cars, float scale, float speed);

// collision.nod (COL_LoadCollisionData 0x004700a0): line segments with the
// kind in the third dword.
struct Segment {
    float x1{};
    float y1{};
    float x2{};
    float y2{};
    int kind{};
};
std::vector<Segment> parseCollision(std::span<const std::uint8_t> bytes);

// path.ai (PATH_LoadRacePath 0x00472000), sorted by position then line
// (0x00472190).
struct PathNode {
    float x{};
    float y{};
    int position{};
    int line{};
    std::uint32_t target{};  // node +0x14, kept as read
};
std::vector<PathNode> parsePath(std::span<const std::uint8_t> bytes);

// sounds.nod (race_LoadSounds 0x00475ae0).
struct SoundEmitter {
    int x{};
    int y{};
    int type{};
};
std::vector<SoundEmitter> parseSounds(std::span<const std::uint8_t> bytes);

// 0x00473120: minutes'seconds"hundredths, or 99'99"99 from 100 minutes.
std::string formatRaceTime(std::uint32_t milliseconds);

} // namespace fgp::racing
