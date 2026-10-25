#pragma once

// The race's car dynamics as FGP.exe integrates them (issue 251): pedals,
// the car-frame velocity integrator, steering, the swept collision pre-pass,
// the free step, wall impacts, damage and revs. Field comments give the
// offsets in the original's 0x29c-byte car record; see "Car dynamics" in
// REVERSE_ENGINEERING.md. Nothing here touches SDL.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "game/racing_data.h"

namespace fgp::racing {

constexpr float kTopSpeedFall = 25.0F;      // 0x0049f260
constexpr float kCatchUpGain = 2.5F;        // 0x0049da6c

// A collision segment as the world keeps it (0x20 bytes): the track's
// lines from collision.nod, then four outline edges per car (kind -3)
// written as each car updates (0x004716d0).
struct WorldSegment {
    float x1{};
    float y1{};
    float x2{};
    float y2{};
    int kind{};
    int owner{};       // +0x14: the car's ini id for an outline, 0 otherwise
    int car{-1};       // +0x1c: the car slot of an outline
};

struct CollisionWorld {
    std::vector<WorldSegment> segments;
    int staticCount{};  // +0xc
    int cursor{};       // +0x1c, reset every frame (0x00470c10)
};

struct PhysicsCar {
    int id{1};                  // +0x000
    bool ai{};                  // +0x1f4
    float accel{};              // +0x108
    float topSpeed{};           // +0x10c
    float boxWidth{};           // +0x118, width times the car size
    float boxHeight{};          // +0x11c
    float braking{};            // +0x120
    float turning{};            // +0x124
    float grip{};               // +0x128
    float mass{};               // +0x12c
    float shield{};             // +0x130
    int gears{4};               // +0x138
    float revs{};               // +0x13c
    float effectiveAccel{};     // +0x140
    float effectiveTop{};       // +0x144
    float reverseAccel{};       // +0x148
    float reverseTop{};         // +0x14c
    float x{};                  // +0x17c
    float y{};                  // +0x180
    float vx{};                 // +0x184
    float vy{};                 // +0x188
    float forward{};            // +0x18c
    float heading{};            // +0x190
    float force{};              // +0x194
    float boost{1.0F};          // +0x198
    bool boosting{};            // +0x19c
    float catchUp{1.0F};        // +0x1a0
    float spin{};               // +0x1a4
    float damage{};             // +0x1a8
    float penalty{1.0F};        // +0x1ac
    bool slip{};                // +0x1f8
    bool reversing{};           // +0x1fc
    bool finished{};            // +0x21c
    int steer{2};               // +0x218: 0 left, 1 right, 2 straight
    float pushX{};              // +0x264
    float pushY{};              // +0x268
    float angular{};            // +0x26c
    bool turned{true};          // +0x270, the last turn fitted
    bool turnedBefore{true};    // +0x274
    bool wasForward{true};      // +0x27c
    // The pre-pass's results (+0x280..+0x294).
    bool contact{};
    bool hit{};
    float hitX{};
    float hitY{};
    bool trigger{};
    int hitKind{};
    // The damage sprite to show (+0x08 from +0x0c + n*4) and whether it
    // changed (+0x3c).
    int sprite{};
    bool spriteChanged{};
    // The terrain entry under the car (+0x210).
    TerrainEntry terrain{};
    // Width of sprite 0, for the outline's forward shift.
    int spriteWidth{};
};

// 0x00476620's reset of the dynamic state.
void resetPhysicsCar(PhysicsCar &car);

// 0x00473a00: damage times the shield, clamped to 0..1, the sprite
// min(damage * 4, 3) and the penalty 1 - damage / 2.
void applyDamage(PhysicsCar &car, float amount);

// 0x00470340 with the collision box (+0x118/+0x11c): the four corners,
// shifted forward by a twelfth of the sprite width and turned by
// -(heading + pi), in world space.
std::array<std::array<float, 2>, 4> carOutline(const PhysicsCar &car, float width,
                                               float height);

// 0x004716d0: the car's outline edges into the world's car slots.
void writeOutline(CollisionWorld &world, const PhysicsCar &car, int slot);

// 0x00471f50: true when no edge of the car's outline crosses a solid
// segment (negative kind) that is not its own.
bool carFits(const CollisionWorld &world, const PhysicsCar &car);

// 0x00471740: sweep the outline along (velocity + push) * dt against the
// segments; on a solid hit advance 0.8 of the way and respond. Returns
// false when the car hit something. A hit on another car's outline calls
// contact(other slot, slide direction) at once, as 0x00471cb0 runs inside
// the loop.
bool sweepCar(CollisionWorld &world, PhysicsCar &car, float dt,
              const std::function<void(int, float, float)> &contact);

// 0x0047a250: position += (velocity + push) * dt, undone if the car then
// overlaps a solid segment. Returns whether it moved.
bool freeStep(const CollisionWorld &world, PhysicsCar &car, float dt);

// 0x00473410, 0x00473860, 0x00473920 then 0x004734b0.
void throttle(PhysicsCar &car, float dt, int raceState);
void brake(PhysicsCar &car, float dt);
void coast(PhysicsCar &car, float dt);
void integrateVelocity(PhysicsCar &car, float dt);

// 0x00479420 with +1 (left), -1 (right) or 0.
void steerCar(const CollisionWorld &world, PhysicsCar &car, int direction, float dt);
inline int steerDirection(int steer) { return steer == 0 ? 1 : steer == 1 ? -1 : 0; }

// 0x00479080: revs toward the next gear (accelerating) or down, clamped
// to the speed when clamp is set.
void updateRevs(PhysicsCar &car, bool accelerating, int gears, bool clamp,
                float top, float dt);
// 0x00478740: wheel spin from the revs at GO (fromRevs), then its decay.
void updateSpin(PhysicsCar &car, bool fromRevs, float dt);
// 0x004792f0: the forward speed, the spin decay and the revs (0x00479330).
void updateForwardSpeed(PhysicsCar &car, float dt);

// 0x004732c0: zero NaN and tiny components.
void sanitizeCar(PhysicsCar &car);

// Step 7 of the frame: the effective top speed and accel.
void updateEffective(PhysicsCar &car, float dt);

} // namespace fgp::racing
