#include "game/racing_physics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace fgp::racing {

namespace {

constexpr float kPi = 3.14159274F;            // 0x0049d620
constexpr float kOutlineShift = 0.0833333358F; // 0x0049ffc0
constexpr float kLateralDamping = 1000.0F;    // 0x0049c78c
constexpr float kCoastDamping = 500.0F;       // 0x004a00d0
constexpr float kSlip = 125.0F;               // 0x004a00d4
constexpr float kSteerDamping = 12.56637F;    // 0x004a0114
constexpr float kMinimumLength2 = 1e-4F;      // 0x0049ffc4
constexpr float kFacing = 0.001F;             // 0x0049c9cc
constexpr float kSlowSweep2 = 625.0F;         // 0x0049ffc8
constexpr float kAdvance = 0.8F;              // 0x0049d9f8
constexpr float kWallDamage = 2.5e-4F;        // 0x3983126f at 0x00471a71
constexpr float kSoftWallDamage = 1.25e-4F;   // 0x3903126f
constexpr float kDamageFloor = 0.0025F;       // 0x0049ffd0
constexpr float kRevStep = 0.01F;             // 0x0049da90

struct Vec {
    float x{};
    float y{};
};

// 0x00470c60: the squared length, at least 1e-4.
float length2(Vec v) {
    const float value = v.y * v.y + v.x * v.x;
    return value <= kMinimumLength2 ? kMinimumLength2 : value;
}

// 0x00470ca0.
Vec normalized(Vec v) {
    const float inverse = 1.0F / std::sqrt(length2(v));
    return {inverse * v.x, inverse * v.y};
}

// 0x00470cf0: the unit normal (y2 - y1, -(x2 - x1)) of a segment.
Vec segmentNormal(Vec a, Vec b) {
    return normalized({b.y - a.y, -(b.x - a.x)});
}

float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y; }

// 0x00473810: rotate (x, y) by angle.
void rotate(float x, float y, float angle, float *outX, float *outY) {
    const double c = std::cos(static_cast<double>(angle));
    const double s = std::sin(static_cast<double>(angle));
    if (outX) *outX = static_cast<float>(c * x - s * y);
    if (outY) *outY = static_cast<float>(c * y + s * x);
}

// 0x00470a20: the point lies on the segment's span, measured along x (or y
// for a vertical segment).
bool withinSegment(const WorldSegment &segment, Vec point) {
    float along = point.x - segment.x1;
    float span = segment.x2 - segment.x1;
    if (span == 0.0F) {
        along = point.y - segment.y1;
        span = segment.y2 - segment.y1;
    }
    const float t = along / span;
    return t <= 1.0F && 0.0F <= t;
}

// 0x00470880: two segments cross, by their lines in slope form. Equal
// slopes count as parallel, which a vertical and a horizontal segment (both
// slope 0) also are.
bool segmentsCross(const WorldSegment &a, const WorldSegment &b) {
    const float aSpan = a.x2 - a.x1;
    const float bSpan = b.x2 - b.x1;
    const float aSlope = aSpan == 0.0F ? 0.0F : (a.y2 - a.y1) / aSpan;
    const float bSlope = bSpan == 0.0F ? 0.0F : (b.y2 - b.y1) / bSpan;
    if (aSlope == bSlope) return false;
    float aIntercept = a.y1 - aSlope * a.x1;
    const float bIntercept = b.y1 - bSlope * b.x1;
    Vec point;
    if (aSpan == 0.0F) {
        point.x = a.x1;
        point.y = point.x * bSlope + bIntercept;
    } else {
        point.x = bSpan == 0.0F ? b.x1 : (bIntercept - aIntercept) / (aSlope - bSlope);
        point.y = point.x * aSlope + aIntercept;
    }
    return withinSegment(a, point) && withinSegment(b, point);
}

std::array<WorldSegment, 4> outlineSegments(const PhysicsCar &car, float width,
                                            float height, int slot) {
    const auto corners = carOutline(car, width, height);
    std::array<WorldSegment, 4> edges;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto &from = corners[index];
        const auto &to = corners[(index + 1) & 3U];
        edges[index] = {from[0], from[1], to[0], to[1], -3, car.id, slot};
    }
    return edges;
}

// The swept quad 0x00471030 builds for one edge: corners q0 (the edge's
// start), q1 = q0 + move, q2 = q3 + move and q3 (the edge's end), with
// the normals of its edge and move vectors for (s, t) coordinates.
struct SweptQuad {
    std::array<Vec, 4> corners;
    Vec edge;          // [0x14] q3 - q0
    Vec move;          // [0x16] q1 - q0
    Vec edgeNormal;    // [0x1c]
    Vec moveNormal;    // [0x1e]
    Vec origin;        // [0x20] q0
};

SweptQuad sweptQuad(Vec from, Vec to, Vec move) {
    SweptQuad quad;
    quad.corners = {from, Vec{from.x + move.x, from.y + move.y},
                    Vec{to.x + move.x, to.y + move.y}, to};
    quad.move = {quad.corners[1].x - quad.corners[0].x, quad.corners[1].y - quad.corners[0].y};
    quad.edge = {quad.corners[3].x - quad.corners[0].x, quad.corners[3].y - quad.corners[0].y};
    quad.moveNormal = segmentNormal({0.0F, 0.0F}, quad.move);
    quad.edgeNormal = segmentNormal({0.0F, 0.0F}, quad.edge);
    quad.origin = quad.corners[0];
    return quad;
}

// A segment end in the quad's (s, t): s along the edge, t along the move,
// and the clip plane that last moved it (-1 for none).
struct QuadPoint {
    float s{};
    float t{};
    int plane{-1};
};

// 0x004712b0.
QuadPoint quadCoordinates(Vec point, const SweptQuad &quad) {
    const Vec d{point.x - quad.origin.x, point.y - quad.origin.y};
    QuadPoint result;
    const Vec back{d.x - quad.move.x, d.y - quad.move.y};
    float first = dot(quad.edgeNormal, d);
    float second = dot(quad.edgeNormal, back);
    result.t = -first / (second - first);
    const Vec side{d.x - quad.edge.x, d.y - quad.edge.y};
    first = dot(quad.moveNormal, d);
    second = dot(quad.moveNormal, side);
    result.s = -first / (second - first);
    return result;
}

// 0x00471440: not both ends past the same side of the unit square.
bool mayOverlap(const QuadPoint &a, const QuadPoint &b) {
    if (a.s < 0.0F && b.s < 0.0F) return false;
    if (1.0F < a.s && 1.0F < b.s) return false;
    if (a.t < 0.0F && b.t < 0.0F) return false;
    if (1.0F < a.t && 1.0F < b.t) return false;
    return true;
}

// 0x00471360: move a toward b onto the planes s >= 0, t <= 1 and s <= 1
// of the table at 0x0050a770. The loop bound (0x0050a79c) leaves out the
// fourth plane, t >= 0. UNCERTAIN: it reads as an original bug; kept.
void clipToQuad(QuadPoint &a, const QuadPoint &b, Vec &worldA, Vec worldB) {
    struct Plane {
        float ns;
        float nt;
        float c;
    };
    // Unit-square boundaries: s >= 0, t <= 1, s <= 1.
    constexpr std::array<Plane, 3> planes{{{1.0F, 0.0F, 0.0F},
                                           {0.0F, -1.0F, -1.0F},
                                           {-1.0F, 0.0F, -1.0F}}};
    for (int index = 0; index < 3; ++index) {
        const auto &plane = planes[static_cast<std::size_t>(index)];
        const float here = a.s * plane.ns + a.t * plane.nt - plane.c;
        if (here > 0.0F) continue;
        const float there = b.s * plane.ns + b.t * plane.nt - plane.c;
        if (there <= 0.0F) return;
        const float t = -(here / (there - here));
        a.s = (b.s - a.s) * t + a.s;
        a.t = (b.t - a.t) * t + a.t;
        worldA.x = (worldB.x - worldA.x) * t + worldA.x;
        worldA.y = (worldB.y - worldA.y) * t + worldA.y;
        a.plane = index;
    }
}

struct SweepResult {
    bool hit{};
    bool trigger{};
    float t{FLT_MAX};
    Vec point;
    Vec direction;
    const WorldSegment *segment{};
    int triggerKind{};
};

// 0x004714e0.
void sweepSegment(SweepResult &result, const WorldSegment &segment, const SweptQuad &quad) {
    if (segment.kind == 0) return;
    Vec first{segment.x1, segment.y1};
    Vec second{segment.x2, segment.y2};
    QuadPoint a = quadCoordinates(first, quad);
    QuadPoint b = quadCoordinates(second, quad);
    a.plane = -1;
    b.plane = -1;
    if (!mayOverlap(a, b)) return;
    clipToQuad(a, b, first, second);
    clipToQuad(b, a, second, first);
    if (!mayOverlap(a, b)) return;
    const bool useSecond = b.t <= a.t;
    const QuadPoint &chosen = useSecond ? b : a;
    const Vec &where = useSecond ? second : first;
    if (!(chosen.t < result.t)) return;
    if (segment.kind < 0) {
        result.segment = &segment;
        result.t = chosen.t;
        result.hit = true;
        result.point = where;
    } else {
        result.trigger = true;
        result.triggerKind = segment.kind;
    }
    if (chosen.plane == -1) {
        result.direction = normalized(quad.edge);
    } else {
        result.direction = normalized({segment.x1 - segment.x2, segment.y1 - segment.y2});
    }
}

// 0x00471c50 through 0x00471c70: 1 - |cos| of the angle between the two.
float glancing(Vec a, Vec b) {
    const Vec na = normalized(a);
    const Vec nb = normalized(b);
    return 1.0F - std::fabs(nb.x * na.x + nb.y * na.y);
}

// 0x00471c00: the vector's projection onto the direction.
void project(float &x, float &y, Vec direction) {
    const float length = std::sqrt(direction.y * direction.y + direction.x * direction.x);
    direction.x /= length;
    direction.y /= length;
    const float along = direction.y * y + direction.x * x;
    x = along * direction.x;
    y = along * direction.y;
}

// 0x00471b10: a wall hit, damage then a slide along the wall.
void wallImpact(PhysicsCar &car, Vec direction, float factor) {
    const Vec moving{car.vx + car.pushX, car.pushY + car.vy};
    const float amount = std::fabs(car.forward) * car.mass * glancing(moving, direction) * factor;
    if (kDamageFloor < amount) applyDamage(car, amount);
    const Vec scaled{direction.x * 0.05F, direction.y * 0.05F};
    project(car.vx, car.vy, scaled);
    project(car.pushX, car.pushY, scaled);
}

// Decay toward zero by step without crossing it.
float decay(float value, float step) {
    if (0.0F <= value) {
        if (0.0F < value) {
            value -= step;
            if (value <= 0.0F) value = 0.0F;
        }
    } else {
        value += step;
        if (0.0F <= value) value = 0.0F;
    }
    return value;
}

} // namespace

void resetPhysicsCar(PhysicsCar &car) {
    car.reverseAccel = car.accel * -0.5F;       // 0x0049e928
    car.effectiveAccel = car.accel;
    car.reverseTop = car.topSpeed * -0.5F;
    car.effectiveTop = car.topSpeed;
    car.finished = false;
    car.damage = 0.0F;
    car.penalty = 1.0F;
    car.boost = 1.0F;
    car.catchUp = 1.0F;
    car.revs = 0.0F;
    car.spin = 0.0F;
    car.pushX = car.pushY = 0.0F;
    car.angular = 0.0F;
    car.force = 0.0F;
    car.forward = 0.0F;
    car.vx = car.vy = 0.0F;
    car.steer = 2;
    car.wasForward = true;
    car.turned = true;
    car.turnedBefore = true;
    car.reversing = false;
    car.boosting = false;
    car.terrain = TerrainEntry{};
    applyDamage(car, -1.0F);  // 0x00476740(car, 1.0)
}

void applyDamage(PhysicsCar &car, float amount) {
    float damage = amount * car.shield + car.damage;
    damage = damage < 0.0F ? 0.0F : std::min(damage, 1.0F);
    car.damage = damage;
    int sprite = static_cast<int>(damage * 4.0F);
    if (sprite > 2) sprite = 3;
    car.sprite = sprite;
    car.spriteChanged = true;
    car.penalty = 1.0F - damage * 0.5F;
}

std::array<std::array<float, 2>, 4> carOutline(const PhysicsCar &car, float width,
                                               float height) {
    const float halfW = width * 0.5F;
    const float halfH = height * 0.5F;
    const float shift = static_cast<float>(car.spriteWidth) * kOutlineShift;
    const std::array<std::array<float, 2>, 4> local{{
        {-halfW, -halfH - shift}, {halfW, -halfH - shift},
        {halfW, halfH - shift}, {-halfW, halfH - shift}}};
    const float angle = -(car.heading + kPi);
    std::array<std::array<float, 2>, 4> corners{};
    for (std::size_t index = 0; index < 4; ++index) {
        float x = 0.0F;
        float y = 0.0F;
        // 0x00470410.
        const double c = std::cos(static_cast<double>(angle));
        const double s = std::sin(static_cast<double>(angle));
        x = static_cast<float>(local[index][0] * c - local[index][1] * s);
        y = static_cast<float>(local[index][0] * s + local[index][1] * c);
        corners[index] = {x + car.x, car.y + y};
    }
    return corners;
}

void writeOutline(CollisionWorld &world, const PhysicsCar &car, int slot) {
    const auto edges = outlineSegments(car, car.boxWidth, car.boxHeight, slot);
    for (const auto &edge : edges) {
        const auto index = static_cast<std::size_t>(world.staticCount + world.cursor);
        if (index < world.segments.size()) world.segments[index] = edge;
        ++world.cursor;
    }
}

bool carFits(const CollisionWorld &world, const PhysicsCar &car) {
    const auto edges = outlineSegments(car, car.boxWidth, car.boxHeight, -1);
    for (const auto &segment : world.segments) {
        if (segment.kind >= 0 || segment.owner == car.id) continue;
        for (const auto &edge : edges) {
            if (segmentsCross(edge, segment)) return false;
        }
    }
    return true;
}

bool sweepCar(CollisionWorld &world, PhysicsCar &car, float dt,
              const std::function<void(int, float, float)> &contact) {
    car.contact = false;
    car.hit = false;
    car.trigger = false;
    for (int iteration = 0;; ++iteration) {
        SweepResult result;
        const Vec move{(car.vx + car.pushX) * dt, (car.vy + car.pushY) * dt};
        const Vec direction = normalized(move);
        if (direction.x == 0.0F && direction.y == 0.0F) return true;
        const auto edges = outlineSegments(car, car.boxWidth, car.boxHeight, -1);
        std::vector<SweptQuad> quads;
        for (const auto &edge : edges) {
            const Vec from{edge.x1, edge.y1};
            const Vec to{edge.x2, edge.y2};
            if (dot(direction, segmentNormal(from, to)) > kFacing)
                quads.push_back(sweptQuad(from, to, move));
        }
        // 0x00471190: the quads' bounding box, then the segments with an
        // end on the inner side of each of its four planes (0x00471260).
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool first = true;
        for (const auto &quad : quads) {
            for (const auto &corner : quad.corners) {
                if (first || corner.x < minX) minX = corner.x;
                if (first || maxX < corner.x) maxX = corner.x;
                if (first || corner.y < minY) minY = corner.y;
                if (first || maxY < corner.y) maxY = corner.y;
                first = false;
            }
        }
        std::vector<const WorldSegment *> candidates;
        if (!quads.empty()) {
            for (const auto &segment : world.segments) {
                if (segment.owner == car.id) continue;
                const auto outside = [](float a, float b, float limit, bool below) {
                    return below ? (a < limit && b < limit) : (-a < -limit && -b < -limit);
                };
                if (outside(segment.x1, segment.x2, minX, true) ||
                    outside(segment.y1, segment.y2, minY, true) ||
                    outside(segment.x1, segment.x2, maxX, false) ||
                    outside(segment.y1, segment.y2, maxY, false)) {
                    continue;
                }
                candidates.push_back(&segment);
            }
        }
        if (!candidates.empty()) {
            for (const auto &quad : quads) {
                for (const auto *segment : candidates) sweepSegment(result, *segment, quad);
            }
            if (result.trigger) {
                car.trigger = true;
                car.hitKind = result.triggerKind;
            }
            if (!result.hit) break;
            if (!car.hit) {
                car.hitX = result.point.x;
                car.hitY = result.point.y;
                car.hitKind = result.segment->kind;
                car.hit = true;
            }
            float advance = result.t;
            if (kSlowSweep2 <= move.x * move.x + move.y * move.y) {
                advance *= kAdvance;
                if (1.0F < advance) break;
            } else {
                advance = 0.0F;
            }
            car.x = move.x * advance + car.x;
            car.y = move.y * advance + car.y;
            const int kind = result.segment->kind;
            if (kind == -3) {
                car.contact = true;
                if (contact) contact(result.segment->car, result.direction.x, result.direction.y);
            } else if (kind == -2) {
                wallImpact(car, result.direction, kSoftWallDamage);
            } else if (kind == -1) {
                wallImpact(car, result.direction, kWallDamage);
            } else {
                car.vx = car.vy = 0.0F;
                car.pushX = car.pushY = 0.0F;
            }
        }
        if (!result.hit || iteration > 2) break;
    }
    sanitizeCar(car);
    return !car.hit;
}

bool freeStep(const CollisionWorld &world, PhysicsCar &car, float dt) {
    const float dx = (car.pushX + car.vx) * dt;
    const float dy = (car.pushY + car.vy) * dt;
    car.x = dx + car.x;
    car.y = dy + car.y;
    if (carFits(world, car)) return true;
    car.x -= dx;
    car.y -= dy;
    return false;
}

namespace {
float traction(const PhysicsCar &car) {
    const float grip = car.terrain.friction * car.grip;
    return 1.0F <= grip ? 1.0F : grip;
}
} // namespace

void throttle(PhysicsCar &car, float dt, int raceState) {
    if (raceState == 2) {
        coast(car, dt);
        return;
    }
    car.reversing = false;
    car.force = car.forward < 0.0F ? car.braking : car.effectiveAccel;
    car.force = traction(car) * car.force;
    integrateVelocity(car, dt);
}

void brake(PhysicsCar &car, float dt) {
    if (car.finished && !car.ai) {
        coast(car, dt);
        return;
    }
    if (0.0F < car.forward) {
        car.force = -car.braking;
    } else {
        car.force = car.reverseAccel;
        car.reversing = true;
    }
    car.force = traction(car) * car.force;
    integrateVelocity(car, dt);
}

void coast(PhysicsCar &car, float dt) {
    const float drag = car.terrain.friction * car.grip * kCoastDamping;
    if (car.forward <= 0.0F) {
        if (car.forward < 0.0F) {
            car.force = drag <= -car.forward ? drag : -car.forward;
        } else {
            car.force = 0.0F;
        }
    } else if (car.forward < drag) {
        car.force = -car.forward;
    } else {
        car.force = -drag;
    }
    integrateVelocity(car, dt);
}

void integrateVelocity(PhysicsCar &car, float dt) {
    float lateral = 0.0F;
    float ahead = 0.0F;
    rotate(car.vx, car.vy, car.heading, &lateral, &ahead);
    float pushLateral = 0.0F;
    float pushAhead = 0.0F;
    rotate(car.pushX, car.pushY, car.heading, &pushLateral, &pushAhead);
    ahead = dt * car.force + ahead;
    const float grip = car.terrain.friction * car.grip;
    if (grip == 0.0F) {
        car.slip = false;
    } else {
        car.slip = car.spin != 0.0F || grip * kSlip < std::fabs(pushLateral + lateral);
    }
    lateral = decay(lateral, dt * grip * kLateralDamping);
    pushLateral = decay(pushLateral, dt * grip * kLateralDamping);
    pushAhead = decay(pushAhead, grip * dt * kCoastDamping);
    if (0.0F < ahead) {
        const float limit =
            std::sqrt(car.effectiveTop * car.effectiveTop - lateral * lateral);
        if (std::isnan(limit)) ahead = 0.0F;
        else if (limit <= ahead) ahead = limit;
    } else if (ahead < 0.0F) {
        const float limit =
            -std::sqrt(car.reverseTop * car.reverseTop - lateral * lateral);
        if (std::isnan(limit)) ahead = 0.0F;
        else if (ahead <= limit) ahead = limit;
    }
    rotate(lateral, ahead, -car.heading, &car.vx, &car.vy);
    rotate(pushLateral, pushAhead, -car.heading, &car.pushX, &car.pushY);
}

void steerCar(const CollisionWorld &world, PhysicsCar &car, int direction, float dt) {
    const float damping = car.terrain.friction * car.grip * dt * kSteerDamping;
    const float turn = (static_cast<float>(direction) * car.turning + car.angular) * dt;
    car.turnedBefore = car.turned;
    car.wasForward = !car.reversing;
    car.heading = turn + car.heading;
    if (carFits(world, car)) {
        car.turned = true;
    } else {
        car.turned = false;
        car.heading -= turn;
    }
    car.angular = decay(car.angular, damping);
}

void updateRevs(PhysicsCar &car, bool accelerating, int gears, bool clamp, float top,
                float dt) {
    const float fraction = std::floor(car.revs) / static_cast<float>(gears);
    float step = (1.0F - fraction) * 0.5F * dt;
    float revs = 0.0F;
    if (!accelerating) {
        revs = car.revs - step;
        if (revs <= 0.0F) revs = 0.0F;
    } else {
        revs = step + car.revs;
        const float ceiling = static_cast<float>(gears) - kRevStep;
        if (ceiling <= revs) revs = ceiling;
    }
    car.revs = revs;
    if (clamp) {
        float bySpeed = std::fabs(car.forward) * static_cast<float>(gears) / top;
        if (car.revs < bySpeed) bySpeed = car.revs;
        car.revs = bySpeed;
    }
    // UNCERTAIN: the engine pitch 0x004791d0 the revs drive is issue 257.
}

void updateSpin(PhysicsCar &car, bool fromRevs, float dt) {
    if (fromRevs) {
        const int gears = car.gears + (car.boosting ? 1 : 0);
        const float top = car.reversing ? car.reverseTop : car.effectiveTop;
        const float spin = (car.revs / static_cast<float>(gears) - car.forward / top) * 1.5F;
        float larger = spin < car.spin ? car.spin : spin;
        float value = 0.0F;
        if (0.0F < larger) value = spin < car.spin ? car.spin : spin;
        car.spin = value;
    }
    if (car.spin != 0.0F) {
        float spin = car.spin - dt * 0.5F;
        if (spin <= 0.0F) spin = 0.0F;
        car.spin = spin;
    }
}

void updateForwardSpeed(PhysicsCar &car, float dt) {
    rotate(car.vx, car.vy, car.heading, nullptr, &car.forward);
    updateSpin(car, false, dt);
    // 0x00479330.
    if (car.reversing) {
        updateRevs(car, !(0.0F <= car.force && car.forward != car.reverseTop), 1, true,
                   -car.reverseTop, dt);
        return;
    }
    const int gears = car.gears + (car.boosting ? 1 : 0);
    updateRevs(car, !(car.force <= 0.0F && car.forward != car.effectiveTop), gears, true,
               car.effectiveTop, dt);
}

void sanitizeCar(PhysicsCar &car) {
    const auto tidy = [](float &value, float floor) {
        if (std::isnan(value) || std::fabs(value) < floor) value = 0.0F;
    };
    tidy(car.force, 1.0F);
    tidy(car.vx, 1.0F);
    tidy(car.vy, 1.0F);
    tidy(car.angular, 0.01F);
    tidy(car.pushX, 1.0F);
    tidy(car.pushY, 1.0F);
}

void updateEffective(PhysicsCar &car, float dt) {
    float top = car.topSpeed;
    if (car.boosting) top *= car.boost;
    top = top * car.penalty * car.catchUp;
    const float falling = car.effectiveTop - dt * kTopSpeedFall;
    car.effectiveTop = top < falling ? falling : top;
    float accel = car.accel;
    if (car.boosting) accel *= car.boost;
    car.effectiveAccel = accel * car.penalty;
}

} // namespace fgp::racing
