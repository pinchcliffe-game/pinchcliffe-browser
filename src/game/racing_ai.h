#pragma once

// The race's opponents (issue 252): the path follower, the AI's throttle,
// steering and recovery, car contact, progress and the position order. See
// "Opponents" in REVERSE_ENGINEERING.md. Nothing here touches SDL.

#include <cstdint>
#include <functional>
#include <vector>

#include "game/racing_physics.h"
#include "util/crt_rand.h"

namespace fgp::racing {

// A path.ai node as PATH_LoadRacePath (0x00472000) keeps it: 0x1c bytes,
// sorted by position then line, then numbered.
struct AiNode {
    int x{};
    int y{};
    int index{};
    int position{};
    int line{};
    float target{};   // +0x14, the speed fraction (and 666 for the part)
    bool pickup{};    // +0x18
};

struct AiPath {
    std::vector<AiNode> nodes;
    int lastPosition{1};  // track +0x68: the last node's position
};

AiPath makeAiPath(const std::vector<PathNode> &nodes);

// A slot's AI record (0x34 bytes at 0x006cad68).
struct AiSlot {
    int target{};          // +0x20, a node index
    int lapPosition{};     // +0x18
    int lapCount{};        // +0x1c
    float viewTimer{2.5F}; // +0x24
    float hornTimer{};     // +0x28
    float revTarget{2.5F}; // +0x2c
    // The far side probes' last car ids (0x004721f0's locals), which the
    // original leaves unwritten when a probe hits nothing.
    int leftOwner{-1};
    int rightOwner{-2};
};

// What the AI reads of a car besides its body.
struct AiCar {
    PhysicsCar *body{};
    AiSlot *slot{};
    CarSpec spec;             // lane weights and bias (+0x150..+0x178)
    int pathLap{1};           // +0x204
    int nodePosition{};       // +0x208
    float reverseTimer{};     // +0x278
    float progress{};         // +0x260
    std::uint32_t raceTime{}; // +0x25c
};

struct AiWorld {
    const AiPath *path{};
    CollisionWorld *collision{};
    std::vector<AiCar> *cars{};
    int laps{3};
    int mode{};
    // The viewport and camera the out-of-view test reads (0x00473260).
    int viewX{};
    int viewY{};
    int viewW{640};
    int viewH{480};
    int cameraX{};
    int cameraY{};
    CrtRandom *random{};
    // Sounds the AI starts: 6 teleport, 8 horn; -8 stops the horn.
    std::function<void(int car, int channel)> sound;
    // Whether the car's horn is still sounding (0x00414240 on +0x44).
    std::function<bool(int car)> hornPlaying;
};

// 0x00472510.
void followPath(AiWorld &world, int car, float dt);
// 0x004721f0 after the follower: nothing for a human car.
void driveAi(AiWorld &world, int car, float dt, int raceState);
// 0x00472740: the next node after `node` for this car.
int nextNode(const AiWorld &world, int node, int car);
// 0x00477ae0.
float raceProgress(const AiWorld &world, int car);
// 0x004771a0: negative when car a goes before car b.
int comparePositions(const AiCar &a, const AiCar &b);
// 0x00471cb0: two cars in contact; dir is the hit's slide direction.
void carContact(PhysicsCar &a, PhysicsCar &b, float dirX, float dirY);

} // namespace fgp::racing
