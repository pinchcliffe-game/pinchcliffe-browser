#include "game/racing_ai.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace fgp::racing {

namespace {

constexpr float kHalfPi = 1.57079637F;     // 0x0049da38
constexpr double kPerSpeed = 1.0 / 350.0;  // 0x0049fff8 (double)
constexpr float kDeadBand = 0.0025F;       // 0x0049ffe0, 0x0049ffd8 (doubles)
constexpr float kNear = 0.75F;             // 0x0049ffe8 (double)
constexpr float kOverSpeed = 0.25F;        // 0x0049fff0 (double)
constexpr float kPitDamage = 0.1F;         // 0x0049db20 (double)
constexpr float kViewTime = 2.5F;          // 0x40200000
constexpr float kContactMass = 350.0F;     // 0x0049eb50
constexpr float kContactDamage = 1.25e-4F; // 0x0049ffd4

int roundAway(float value) {
    return static_cast<int>(0.0F < value ? value + 0.5F : value - 0.5F);
}

// 0x00473a90: damaged past 0.1 and not on the last lap: off to the pits.
bool headsForPit(const AiWorld &world, const AiCar &car) {
    return kPitDamage < car.body->damage && car.pathLap < world.laps;
}

// 0x00473260 through 0x004731e0: the car's rounded position lies in the
// viewport as drawn last. Mode 1 counts every car as out of view.
bool inView(const AiWorld &world, const PhysicsCar &car) {
    if (world.mode == 1) return false;
    const int x = roundAway(car.x) + world.viewX - world.cameraX;
    const int y = roundAway(car.y) + world.viewY - world.cameraY;
    return world.viewX <= x && x < world.viewX + world.viewW && world.viewY <= y &&
           y < world.viewY + world.viewH;
}

// 0x00472970: a probe length from the speed: size * (|forward| * length *
// share / 350 + (1 - share) * length).
int probeLength(const PhysicsCar &car, int length, float share, float size) {
    const double value = std::fabs(car.forward) * (static_cast<double>(length) * share) *
                             kPerSpeed +
                         (1.0 - share) * static_cast<double>(length);
    return static_cast<int>(size * value);
}

// 0x00472cf0: an angle from wide at rest to narrow at speed 350.
float probeAngle(const PhysicsCar &car, float narrow, float wide) {
    const double value = wide - std::fabs(car.forward) * (wide - narrow) * kPerSpeed;
    if (value < narrow) return narrow;
    if (wide < value) return wide;
    return static_cast<float>(value);
}

struct ProbeHit {
    int kind{};          // 1 pickup, 2 car, 3 wall, 4 trick
    float fraction{1.0F};
    int x{};
    int y{};
    int owner{};
};

// 0x00470880 with the crossing point (0x00474480 reads it).
bool crossing(float ax1, float ay1, float ax2, float ay2, const WorldSegment &b, float &x,
              float &y) {
    const float aSpan = ax2 - ax1;
    const float bSpan = b.x2 - b.x1;
    const float aSlope = aSpan == 0.0F ? 0.0F : (ay2 - ay1) / aSpan;
    const float bSlope = bSpan == 0.0F ? 0.0F : (b.y2 - b.y1) / bSpan;
    if (aSlope == bSlope) return false;
    const float aIntercept = ay1 - aSlope * ax1;
    const float bIntercept = b.y1 - bSlope * b.x1;
    if (aSpan == 0.0F) {
        x = ax1;
        y = x * bSlope + bIntercept;
    } else {
        x = bSpan == 0.0F ? b.x1 : (bIntercept - aIntercept) / (aSlope - bSlope);
        y = x * aSlope + aIntercept;
    }
    const auto within = [&](float x1, float y1, float x2, float y2) {
        float along = x - x1;
        float span = x2 - x1;
        if (span == 0.0F) {
            along = y - y1;
            span = y2 - y1;
        }
        const float t = along / span;
        return t <= 1.0F && 0.0F <= t;
    };
    return within(ax1, ay1, ax2, ay2) && within(b.x1, b.y1, b.x2, b.y2);
}

// 0x00470600: a segment's bounding box from its rounded ends.
std::array<int, 4> roundedBox(float x1, float y1, float x2, float y2) {
    const int a = roundAway(x1), b = roundAway(x2), c = roundAway(y1), d = roundAway(y2);
    return {std::min(a, b), std::min(c, d), std::max(a, b), std::max(c, d)};
}

// 0x004740d0: the car with id `other` heads the same way (unit velocities'
// dot at least 0.5) and is slower than the car with id `self`.
bool catchingUp(const AiWorld &world, int self, int other) {
    const PhysicsCar *me = nullptr;
    const PhysicsCar *them = nullptr;
    for (const auto &car : *world.cars) {
        if (car.body->id == self) me = car.body;
        else if (car.body->id == other) them = car.body;
    }
    if (!me || !them) return false;
    const float meLength = std::sqrt(me->vy * me->vy + me->vx * me->vx);
    const float themLength = std::sqrt(them->vy * them->vy + them->vx * them->vx);
    const float dot = (them->vx / themLength) * (me->vx / meLength) +
                      (them->vy / themLength) * (me->vy / meLength);
    return 0.5F <= dot && them->forward < me->forward;
}

// 0x004729b0 then 0x004741b0: a ray from the car at heading - pi/2 + angle.
// UNCERTAIN: the pickups (0x004745c0) and the oil and spike hazards
// (0x00474320) it can also meet are issue 254; only segments are tested.
bool probe(const AiWorld &world, const PhysicsCar &car, float angle, int length,
           bool segments, ProbeHit &hit) {
    hit = ProbeHit{};
    const double direction = static_cast<double>(car.heading) - kHalfPi + angle;
    const float x0 = car.x;
    const float y0 = car.y;
    const auto x1 = static_cast<float>(std::cos(direction) * length + car.x);
    const auto y1 = static_cast<float>(car.y - std::sin(direction) * length);
    if (segments) {
        const auto box = roundedBox(x0, y0, x1, y1);
        for (const auto &segment : world.collision->segments) {
            // 0x00474480.
            if (segment.owner == car.id || segment.kind >= 0) continue;
            const auto other = roundedBox(segment.x1, segment.y1, segment.x2, segment.y2);
            if (!(box[0] <= other[2] && box[1] <= other[3] && other[0] <= box[2] &&
                  other[1] <= box[3]))
                continue;
            float cx = 0.0F;
            float cy = 0.0F;
            if (!crossing(x0, y0, x1, y1, segment, cx, cy)) continue;
            ProbeHit found;
            found.x = roundAway(cx);
            found.y = roundAway(cy);
            const float dx = static_cast<float>(found.x) - x0;
            const float dy = static_cast<float>(found.y) - y0;
            found.fraction = std::sqrt(dy * dy + dx * dx) / static_cast<float>(length);
            found.owner = segment.owner;
            found.kind = segment.kind == -3 ? 2 : 3;
            if (!(found.fraction < hit.fraction)) continue;
            if (segment.owner < 1) {
                hit = found;
            } else if (catchingUp(world, car.id, segment.owner)) {
                hit = found;
                hit.fraction *= 0.25F;
            }
        }
    }
    return hit.kind != 0;
}

float biasFor(const CarSpec &spec, int kind) {
    switch (kind) {
    case 1: return spec.pickup;
    case 2: return spec.car;
    case 3: return spec.wall;
    case 4: return spec.trick;
    default: return 0.0F;
    }
}

// 0x00472ec0: whether the side toward `side` (+1 or -1) is clear of cars.
// The loop only ever compares the car with itself (0x00472f3e), so the
// test reduces to the rounding residue of (float(sin) * x + float(cos) * y)
// against the same in the FPU's precision: 0 < residue < 50 means blocked.
// UNCERTAIN: the residue depends on the x87 precision in force; native
// assumes the C runtime's default 53-bit precision.
bool sideClear(const PhysicsCar &car, int side) {
    // 0x00472ec0 compares the car only with itself (0x00472f36 skips every
    // other record), so it measures rounding: the projection
    // cos(a) * y + float(sin(a)) * x is stored as a float (0x00472f0e), then
    // float(cos(a)) * y + float(sin(a)) * x less that float (0x00472fb2-
    // 0x00472fc8) must not lie in (0, 50) (0x0049d9f4). The reference host's
    // x87 runs at extended precision: at the grid this reports "not clear"
    // and the look-ahead pull is a tenth (logged at 0x004724a2), so double
    // stands in for extended here.
    const double angle = side * static_cast<double>(kHalfPi) + car.heading;
    const double s = std::sin(angle);
    const double c = std::cos(angle);
    const auto sf = static_cast<double>(static_cast<float>(s));
    const auto cf = static_cast<double>(static_cast<float>(c));
    const auto stored = static_cast<double>(static_cast<float>(c * car.y + sf * car.x));
    const double residue = (cf * car.y + sf * car.x) - stored;
    return !(0.0 < residue && residue < 50.0);
}

// 0x00472d40: one side probe's steering pull.
float sideProbe(AiWorld &world, int index, float angle, int length, int *owner) {
    auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    ProbeHit hit;
    if (!probe(world, *car.body, angle, length, true, hit)) return 0.0F;
    if (hit.kind == 2 && hit.fraction < car.spec.horn && car.slot->hornTimer == 0.0F) {
        if (world.sound) world.sound(index, 8);
        car.slot->hornTimer = static_cast<float>(world.random->below(0x4b)) * 0.01F + 0.25F;
    }
    if (owner) *owner = hit.owner;
    return (1.0F - hit.fraction) * (25.0F / static_cast<float>(length)) *
           biasFor(car.spec, hit.kind);
}

// 0x00472e00 (side +1) and 0x00472c40 (side -1): a near and a far probe.
float sideProbes(AiWorld &world, int index, int side, int *owner, float size) {
    const PhysicsCar &car = *(*world.cars)[static_cast<std::size_t>(index)].body;
    const int nearLength = probeLength(car, 0x1e, 0.25F, size);
    const float nearAngle = probeAngle(car, 1.04719758F, 1.57079637F);
    const float nearPull = sideProbe(world, index, static_cast<float>(side) * nearAngle,
                                     nearLength, nullptr);
    const int farLength = probeLength(car, 0x96, 0.6F, size);
    const float farAngle = probeAngle(car, 0.125663713F, 0.785398185F);
    const float farPull = sideProbe(world, index, static_cast<float>(side) * farAngle,
                                    farLength, owner);
    const float pull = farPull * 0.75F + nearPull;
    return sideClear(car, -side) ? pull : pull * 0.1F;
}

// 0x00472a50: the pull toward the racing line some nodes ahead, weighted by
// the track bias.
float lookAhead(AiWorld &world, int index) {
    auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    const PhysicsCar &self = *car.body;
    // 0x00472b80: 1 - (int)(forward / top * -8) nodes summed, normalised.
    const int count = 1 - static_cast<int>(self.forward / self.effectiveTop * -8.0F);
    float tx = 0.0F;
    float ty = 0.0F;
    int node = car.slot->target;
    for (int step = 0; step < count; ++step) {
        const auto &at = world.path->nodes[static_cast<std::size_t>(node)];
        tx = (static_cast<float>(at.x) - self.x) + tx;
        ty = (static_cast<float>(at.y) - self.y) + ty;
        node = nextNode(world, node, index);
    }
    const float length = std::sqrt(tx * tx + ty * ty);
    tx /= length;
    ty /= length;
    const double angle = static_cast<double>(self.heading) - kHalfPi;
    const auto c = static_cast<float>(std::cos(angle));
    const auto s = static_cast<float>(std::sin(angle));
    const float across = tx * -s - ty * c;
    const float along = tx * c + ty * -s;
    float pull = 0.0F;
    if (0.0F <= across) {
        if (across <= 0.0F) return 0.0F;
        pull = -((1.0F - along) * car.spec.trackWeight);
    } else {
        pull = (1.0F - along) * car.spec.trackWeight;
    }
    if (0.0F < pull && !sideClear(self, -1)) pull *= 0.1F;
    if (pull < 0.0F && !sideClear(self, 1)) return pull * 0.1F * 0.05F;
    return pull * 0.05F;
}

// 0x004728d0: hold the node's speed fraction.
void holdSpeed(AiWorld &world, int index, int raceState, float dt) {
    auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    PhysicsCar &self = *car.body;
    const float target = world.path->nodes[static_cast<std::size_t>(car.slot->target)].target;
    if (0.0F < target) {
        const float fraction = std::clamp(target, 0.1F, 1.0F);
        const float over = self.forward / self.effectiveTop - fraction;
        if (0.0F <= over) {
            if (kOverSpeed <= over) brake(self, dt);
            else coast(self, dt);
            return;
        }
    }
    throttle(self, dt, raceState);
}

} // namespace

AiPath makeAiPath(const std::vector<PathNode> &nodes) {
    AiPath path;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        AiNode node;
        node.x = static_cast<int>(nodes[index].x);
        node.y = static_cast<int>(nodes[index].y);
        node.index = static_cast<int>(index);
        node.position = nodes[index].position;
        node.line = nodes[index].line;
        std::uint32_t bits = nodes[index].target;
        float value = 0.0F;
        static_assert(sizeof value == sizeof bits);
        std::memcpy(&value, &bits, sizeof value);
        node.target = value;
        path.nodes.push_back(node);
    }
    if (!path.nodes.empty()) path.lastPosition = path.nodes.back().position;
    if (path.lastPosition <= 0) path.lastPosition = 1;
    return path;
}

int nextNode(const AiWorld &world, int node, int car) {
    const auto &nodes = world.path->nodes;
    const int count = static_cast<int>(nodes.size());
    const auto &from = nodes[static_cast<std::size_t>(node)];
    const auto &self = (*world.cars)[static_cast<std::size_t>(car)];
    const int line = headsForPit(world, self) ? 2 : from.line;
    const int start = from.index + 1;
    int index = start;
    for (; index < count; ++index) {
        const auto &candidate = nodes[static_cast<std::size_t>(index)];
        if (from.position + 1 < candidate.position) break;
        if (from.position < candidate.position && candidate.line == line) return index;
    }
    if (index == count) {
        for (int first = 0; first < 4 && first < count; ++first)
            if (nodes[static_cast<std::size_t>(first)].line == line) return first;
    }
    if (count <= start) return 0;
    for (int next = start; next < count; ++next) {
        const auto &candidate = nodes[static_cast<std::size_t>(next)];
        if (from.position < candidate.position && candidate.line == 0) return next;
    }
    return 0;
}

void followPath(AiWorld &world, int index, float dt) {
    auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    auto &slot = *car.slot;
    PhysicsCar &self = *car.body;
    const auto &nodes = world.path->nodes;
    const int last = world.path->lastPosition;
    const int current = nodes[static_cast<std::size_t>(slot.target)].position;
    const int low = (last - 0xc + current) % last;
    const int high = (current + 0xc) % last;
    // 0x00472850: distance from the truncated position times the line's
    // weight; the best node within 12 positions either way.
    const int carX = static_cast<int>(self.x);
    const int carY = static_cast<int>(self.y);
    float best = FLT_MAX;
    for (const auto &node : nodes) {
        bool near = false;
        if (low < high) near = low <= node.position && node.position <= high;
        else near = node.position < low ? node.position <= high : true;
        if (!near) continue;
        const int dx = node.x - carX;
        const int dy = node.y - carY;
        const auto distance = static_cast<float>(std::sqrt(static_cast<double>(dy * dy + dx * dx)));
        const float weight = node.line >= 0 && node.line < 4
                                 ? car.spec.lane[static_cast<std::size_t>(node.line)]
                                 : 0.0F;
        const float score = distance * weight;
        if (score < best) {
            best = score;
            slot.target = node.index;
        }
    }
    const int reached = nodes[static_cast<std::size_t>(slot.target)].position;
    const int next = nextNode(world, slot.target, index);
    slot.target = next;
    const int ahead = nodes[static_cast<std::size_t>(next)].position;
    if (ahead != slot.lapPosition) {
        if (slot.lapPosition < ahead && ahead < slot.lapPosition + 0x12) slot.lapPosition = ahead;
        if (last - 0x12 <= slot.lapPosition && ahead < reached) {
            slot.lapPosition = ahead;
            ++slot.lapCount;
        }
    }
    if (ahead != car.nodePosition) slot.viewTimer = kViewTime;
    // 0x004725d0-0x004726ee: an AI car out of view for 2.5 s with its
    // target not reached jumps to the target if it fits there.
    if (self.ai && self.damage < 1.0F && !inView(world, self)) {
        slot.viewTimer -= dt;
        if (slot.viewTimer <= 0.0F) {
            const float oldX = self.x;
            const float oldY = self.y;
            self.x = static_cast<float>(nodes[static_cast<std::size_t>(slot.target)].x);
            self.y = static_cast<float>(nodes[static_cast<std::size_t>(slot.target)].y);
            if (!carFits(*world.collision, self)) {
                self.x = oldX;
                self.y = oldY;
            } else if (world.sound) {
                world.sound(index, 6);
            }
            slot.viewTimer = kViewTime;
        }
    }
    const int before = car.nodePosition;
    car.nodePosition = ahead;
    if (last - 0x18 <= before && car.nodePosition < 0x19) {
        ++car.pathLap;
        return;
    }
    if (last - 0x18 <= car.nodePosition && before < 0x19) --car.pathLap;
}

void driveAi(AiWorld &world, int index, float dt, int raceState) {
    followPath(world, index, dt);
    auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    PhysicsCar &self = *car.body;
    if (!self.ai) return;
    auto &slot = *car.slot;
    if (1.0F <= self.damage) {
        coast(self, dt);
        return;
    }
    // Recovery: a turn that failed twice, or a car contact, reverses for a
    // random time.
    if (car.reverseTimer <= 0.0F) {
        if (!self.turned && !self.turnedBefore)
            car.reverseTimer = static_cast<float>(world.random->below(0x28)) * 0.01F + 0.1F;
        if (self.contact)
            car.reverseTimer = static_cast<float>(world.random->below(10)) * 0.01F + 0.1F;
    }
    if (0.0F < car.reverseTimer) {
        car.reverseTimer = car.reverseTimer - dt <= 0.0F ? 0.0F : car.reverseTimer - dt;
        self.steer = 2;
        if (self.wasForward) brake(self, dt);
        else throttle(self, dt, raceState);
        return;
    }
    // The horn timer: when it runs out, a sounding horn stops and the next
    // honk waits 5 to 10 s.
    slot.hornTimer -= dt <= slot.hornTimer ? dt : slot.hornTimer;
    if (slot.hornTimer == 0.0F && world.hornPlaying && world.hornPlaying(index)) {
        if (world.sound) world.sound(index, -8);
        slot.hornTimer = static_cast<float>(world.random->below(500)) * 0.01F + 5.0F;
    }
    // Throttle: in the pits while repairing, coast; else a forward probe.
    const float size = 0.85F;  // 0x0050a970 through 0x00473070
    if (!headsForPit(world, car) || self.terrain.repair <= 0.0F) {
        ProbeHit hit;
        const bool met = probe(world, self, 0.0F, probeLength(self, 200, 0.8F, size), true, hit);
        if (!met || biasFor(car.spec, hit.kind) < 1.0F) {
            holdSpeed(world, index, raceState, dt);
        } else if (hit.fraction < kNear || self.forward < 25.0F) {
            brake(self, dt);
        } else {
            coast(self, dt);
        }
    } else {
        coast(self, dt);
    }
    // Steering.
    float pull = lookAhead(world, index);
    const float right = sideProbes(world, index, 1, &slot.rightOwner, size);
    const float left = sideProbes(world, index, -1, &slot.leftOwner, size);
    if (slot.leftOwner != slot.rightOwner) pull = (right - left) + pull;
    if (kDeadBand < pull) self.steer = 1;
    else if (pull < -kDeadBand) self.steer = 0;
    else self.steer = 2;
}

float raceProgress(const AiWorld &world, int index) {
    const auto &car = (*world.cars)[static_cast<std::size_t>(index)];
    const auto &target = world.path->nodes[static_cast<std::size_t>(car.slot->target)];
    const float dx = static_cast<float>(target.x) - car.body->x;
    const float dy = static_cast<float>(target.y) - car.body->y;
    float length = dy * dy + dx * dx;
    if (length <= 1e-4F) length = 1e-4F;
    float distance = std::sqrt(length);
    if (distance <= 25.0F) distance = 25.0F;
    float progress = ((static_cast<float>(car.nodePosition) - distance * 0.04F) /
                          static_cast<float>(world.path->lastPosition) +
                      static_cast<float>(car.pathLap - 1)) /
                     static_cast<float>(world.laps);
    if (progress < 0.0F) return 0.0F;
    if (1.0F < progress) progress = 1.0F;
    return progress;
}

int comparePositions(const AiCar &a, const AiCar &b) {
    const PhysicsCar &first = *a.body;
    const PhysicsCar &second = *b.body;
    if (second.finished && first.finished) {
        if (b.raceTime < a.raceTime) return 1;
        if (a.raceTime < b.raceTime) return -1;
        return second.id <= first.id ? -1 : 1;
    }
    if (!second.finished) {
        if (first.finished) return -1;
    } else if (!first.finished) {
        return 1;
    }
    if (second.damage < 1.0F && first.damage == 1.0F) return 1;
    if (second.damage == 1.0F && first.damage < 1.0F) return -1;
    if (a.progress < b.progress) return 1;
    if (b.progress < a.progress) return -1;
    return second.id <= first.id ? -1 : 1;
}

void carContact(PhysicsCar &a, PhysicsCar &b, float dirX, float dirY) {
    const float avx = a.pushX + a.vx;
    const float avy = a.pushY + a.vy;
    const float bvx = b.pushX + b.vx;
    const float bvy = b.pushY + b.vy;
    const float aAlong = avy * dirY + avx * dirX;
    const float bAlong = bvy * dirY + bvx * dirX;
    const float aAcross = dirY * avx + -dirX * avy;
    const float bAcross = dirY * bvx + -dirX * bvy;
    const float aMass = a.mass * kContactMass;
    const float bMass = b.mass * kContactMass;
    const float aMomentum = aMass * aAcross;
    const float bMomentum = bMass * bAcross;
    const float total = aMomentum + bMomentum;
    const float aAfter = ((aMomentum - bMomentum) * 0.05F + total) / (aMass + aMass);
    const float bAfter = (total - aAfter * aMass) / bMass;
    float ax = aAfter * dirY + aAlong * dirX;
    float ay = aAlong * dirY + aAfter * -dirX;
    const float bx = bAfter * dirY + bAlong * dirX;
    const float by = bAlong * dirY + bAfter * -dirX;
    if (0.0F <= ax) {
        if (bx <= ax) ax = bx;
    } else if (bx >= ax) {
        ax = bx;
    }
    if (0.0F <= ay) {
        if (by <= ay) ay = by;
    } else if (by >= ay) {
        ay = by;
    }
    // 0x00471ee0: the velocity becomes the part along the heading, the push
    // the rest.
    const auto split = [](PhysicsCar &car, float x, float y) {
        float hx = static_cast<float>(std::sin(static_cast<double>(car.heading)));
        float hy = static_cast<float>(std::cos(static_cast<double>(car.heading)));
        const float length = std::sqrt(hy * hy + hx * hx);
        hx /= length;
        hy /= length;
        const float along = hy * y + hx * x;
        car.vx = along * hx;
        car.vy = along * hy;
        car.pushX = x - car.vx;
        car.pushY = y - car.vy;
    };
    split(a, ax, ay);
    split(b, bx, by);
    // UNCERTAIN: the sign of this damage term for cars closing on each
    // other (REVERSE_ENGINEERING.md, Car dynamics).
    const float damage = (aAcross - bAcross) * kContactDamage;
    applyDamage(a, (b.mass / a.mass) * damage);
    applyDamage(b, (a.mass / b.mass) * damage);
}

} // namespace fgp::racing
