#include "game/racing_data.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "assets/language_table.h"
#include "util/binary.h"

namespace fgp::racing {

namespace {

// 0x0049f650 (0x3c8efa35). 0x004754d0 and 0x00475e75 multiply by it on the
// x87 at extended precision and store a float, so 90 degrees is exactly
// 0x3fc90fdb (float pi/2); a single-precision product is one ulp short.
constexpr float kDegreesToRadians = 0.01745329238474369F;
float degreesToRadians(float degrees) {
    return static_cast<float>(static_cast<double>(degrees) *
                              static_cast<double>(kDegreesToRadians));
}

// The ini readers 0x0040e3e0 and 0x0040e3a0 sscanf the stated value with
// "%f" (0x004a6f8c) and "%d" (0x004a6f88); a value that does not scan
// leaves the default.
float iniFloat(const LanguageTable &ini, const std::string &section,
               const std::string &key, float fallback) {
    const std::string text = ini.get(section, key, "\x01");
    if (text == "\x01") return fallback;
    float value = fallback;
    if (std::sscanf(text.c_str(), "%f", &value) != 1) return fallback;
    return value;
}

int iniInt(const LanguageTable &ini, const std::string &section,
           const std::string &key, int fallback) {
    const std::string text = ini.get(section, key, "\x01");
    if (text == "\x01") return fallback;
    int value = fallback;
    if (std::sscanf(text.c_str(), "%d", &value) != 1) return fallback;
    return value;
}

} // namespace

RaceBlock storyRaceBlock() {
    RaceBlock block;
    block.track = "flaaklypa";
    block.cars = {"tempo", "rapido", "white", "no5"};
    block.laps = 3;
    block.cars_count = 4;
    block.mode = 0;
    // UNCERTAIN: the story block's tricks word is 0x001b7740, nonzero.
    block.tricks = 0x001b7740U;
    block.story = true;
    return block;
}

void drawRaceCars(RaceBlock &block, int car1Resource, int car2Resource,
                  int car1Index, int car2Index,
                  const std::function<std::string(int)> &carResourceName,
                  CrtRandom &random) {
    int chosen = 1;
    block.cars[0] = carResourceName(car1Resource);
    // 0x0047ab30: modes 1 and 2.
    if (block.cars_count > 1 && block.mode > 0 && block.mode <= 2) {
        block.cars[1] = carResourceName(car2Resource);
        chosen = 2;
    }
    std::vector<int> pool{0, 1, 2, 3, 4, 5, 6};
    // 0x0040a0f0: the index of the first element equal to the key, or -1.
    const auto find = [&](int key) {
        const auto found = std::find(pool.begin(), pool.end(), key);
        return found == pool.end() ? -1 : static_cast<int>(found - pool.begin());
    };
    // 0x0040a0b0: memmove the tail down over the element; an index of -1
    // moves the whole pool down one, dropping its first entry.
    const auto removeAt = [&](int index) {
        if (index < 0) {
            pool.erase(pool.begin());
        } else {
            pool.erase(pool.begin() + index);
        }
    };
    if (chosen == 2) removeAt(find(car2Index));
    if (chosen >= 1) {
        const int index = find(car1Index);
        // The second removal is skipped once the pool count went negative
        // (0x0047c1f5), which seven entries never reach.
        removeAt(index);
    }
    // 0x004120e0: draw count entries without repeats.
    std::vector<int> drawn;
    std::vector<int> source = pool;
    for (int left = static_cast<int>(source.size()); left > 0; --left) {
        const int pick = random.next() % left;
        drawn.push_back(source[static_cast<std::size_t>(pick)]);
        source.erase(source.begin() + pick);
    }
    for (int slot = chosen; slot < block.cars_count && slot < 4; ++slot) {
        block.cars[static_cast<std::size_t>(slot)] =
            carResourceName(drawn[static_cast<std::size_t>(slot - chosen)]);
    }
}

TrackSpec parseTrackIni(std::span<const std::uint8_t> bytes) {
    const LanguageTable ini(bytes);
    TrackSpec spec;
    spec.id = iniInt(ini, "track", "id", -1);
    spec.award = iniInt(ini, "track", "award", -1);
    spec.difficulty = iniFloat(ini, "track", "difficulty", 1.0F);
    spec.speed = iniFloat(ini, "track", "speed", 1.0F);
    spec.track = ini.get("track", "track");
    for (int index = 0; index < 4; ++index) {
        spec.startX[static_cast<std::size_t>(index)] =
            iniInt(ini, "track", "startx" + std::to_string(index + 1), 0);
        spec.startY[static_cast<std::size_t>(index)] =
            iniInt(ini, "track", "starty" + std::to_string(index + 1), 0);
    }
    spec.startAngle = degreesToRadians(iniFloat(ini, "track", "startAngle", 0.0F));
    spec.terrain = ini.get("track", "terrain");
    for (int index = 0; index < 16; ++index) {
        const std::string section = "terrain" + std::to_string(index);
        auto &entry = spec.terrains[static_cast<std::size_t>(index)];
        entry.friction = iniFloat(ini, section, "friction", 1.0F);
        entry.pitch = iniFloat(ini, section, "pitch", 1.0F);
        entry.volume = iniFloat(ini, section, "volume", 1.0F);
        entry.repair = iniFloat(ini, section, "repair", 0.0F);
    }
    spec.map = ini.get("track", "map");
    spec.sounds = ini.get("track", "sounds");
    spec.collision = ini.get("track", "collision");
    spec.path = ini.get("track", "path");
    const int overlays = iniInt(ini, "track", "overlays", 0);
    for (int index = 1; index <= overlays; ++index) {
        TrackOverlay overlay;
        overlay.x = iniInt(ini, "overlays", "x" + std::to_string(index), 0);
        overlay.y = iniInt(ini, "overlays", "y" + std::to_string(index), 0);
        overlay.source = ini.get("overlays", "source" + std::to_string(index));
        overlay.ontop = iniInt(ini, "overlays", "ontop" + std::to_string(index), 0);
        spec.overlays.push_back(std::move(overlay));
    }
    return spec;
}

CarSpec parseCarIni(std::span<const std::uint8_t> bytes, bool ai) {
    const LanguageTable ini(bytes);
    CarSpec spec;
    spec.ai = ai;
    spec.accel = iniFloat(ini, "car", "accel", 350.0F);
    spec.topSpeed = iniFloat(ini, "car", "topSpeed", 350.0F);
    spec.braking = iniFloat(ini, "car", "braking", 700.0F);
    spec.width = iniFloat(ini, "car", "width", 0.0F);
    spec.height = iniFloat(ini, "car", "height", 0.0F);
    spec.turning = degreesToRadians(iniFloat(ini, "car", "turning", 180.0F));
    spec.grip = iniFloat(ini, "car", "grip", 0.8F);
    spec.mass = iniFloat(ini, "car", "mass", 1.0F);
    spec.shield = iniFloat(ini, "car", "shield", 1.0F);
    spec.gears = iniInt(ini, "car", "gears", 4);
    spec.difficulty = iniFloat(ini, "car", "difficulty", 1.0F);
    spec.id = iniInt(ini, "car", "id", 1);
    if (ai) {
        spec.lane[0] = iniFloat(ini, "bias", "normal", 1.0F);
        spec.lane[1] = iniFloat(ini, "bias", "sneak", 2.5F);
        spec.lane[2] = iniFloat(ini, "bias", "damaged", 2.0F);
        spec.pickup = iniFloat(ini, "bias", "pickup", -1.0F);
        spec.car = iniFloat(ini, "bias", "car", 1.0F);
        spec.wall = iniFloat(ini, "bias", "wall", 1.0F);
        spec.trick = iniFloat(ini, "bias", "trick", 1.0F);
        spec.trackWeight = iniFloat(ini, "bias", "track", 1.0F);
        spec.horn = iniFloat(ini, "bias", "horn", 0.75F);
        // 0x0049d9f8 at 0x00475feb.
        spec.grip *= 0.8F;
    }
    return spec;
}

bool usesStageFile(const std::string &resource) {
    // The compare runs on the whole "%s/racing/cars/%s" path, which is
    // always longer than five characters.
    return resource.size() >= 5 &&
           resource.compare(resource.size() - 5, 5, "tempo") == 0;
}

float loadScale(int mode, const TrackSpec &track,
                const std::vector<CarSpec> &cars) {
    float player = 1.0F;
    if (mode == 0) {
        if (!cars.empty()) player = cars[0].difficulty;
    } else if (mode == 1) {
        return 1.1F;
    } else if (mode == 2 && cars.size() >= 2) {
        player = (cars[1].difficulty + cars[0].difficulty) * 0.5F;
    }
    float excess = track.difficulty - player;
    if (excess <= 0.0F) excess = 0.0F;
    return excess + 1.0F;
}

void applyLoadScaling(std::vector<CarSpec> &cars, float scale, float speed) {
    for (auto &car : cars) {
        if (!car.ai) continue;
        car.accel = scale * car.accel;
        car.topSpeed = scale * car.topSpeed;
        car.braking = scale * car.braking;
        car.turning = scale * car.turning;
        car.grip = scale * scale * scale * car.grip;
        car.mass = scale * car.mass;
    }
    for (auto &car : cars) {
        car.accel = speed * car.accel;
        car.topSpeed = speed * car.topSpeed;
    }
}

std::vector<Segment> parseCollision(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) throw std::runtime_error("collision.nod: no header");
    const auto points = static_cast<std::size_t>(readLe32(bytes, 0));
    const auto lines = static_cast<std::size_t>(readLe32(bytes, 4));
    const std::size_t lineStart = 8 + points * 16;
    if (bytes.size() < lineStart + lines * 12)
        throw std::runtime_error("collision.nod: short file");
    std::vector<Segment> segments;
    segments.reserve(lines);
    const auto point = [&](std::uint32_t index, float &x, float &y) {
        if (index >= points) throw std::runtime_error("collision.nod: bad index");
        x = static_cast<float>(static_cast<std::int32_t>(readLe32(bytes, 8 + index * 16)));
        y = static_cast<float>(
            static_cast<std::int32_t>(readLe32(bytes, 8 + index * 16 + 4)));
    };
    for (std::size_t line = 0; line < lines; ++line) {
        const std::size_t at = lineStart + line * 12;
        Segment segment;
        point(readLe32(bytes, at), segment.x1, segment.y1);
        point(readLe32(bytes, at + 4), segment.x2, segment.y2);
        segment.kind = static_cast<std::int32_t>(readLe32(bytes, at + 8));
        segments.push_back(segment);
    }
    return segments;
}

std::vector<PathNode> parsePath(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) throw std::runtime_error("path.ai: no header");
    const auto count = static_cast<std::size_t>(readLe32(bytes, 0));
    if (bytes.size() < 8 + count * 16) throw std::runtime_error("path.ai: short file");
    std::vector<PathNode> nodes;
    nodes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t at = 8 + index * 16;
        PathNode node;
        node.x = static_cast<float>(static_cast<std::int32_t>(readLe32(bytes, at)));
        node.y = static_cast<float>(static_cast<std::int32_t>(readLe32(bytes, at + 4)));
        const std::uint32_t place = readLe32(bytes, at + 8);
        node.position = static_cast<std::int16_t>(place >> 16);
        node.line = static_cast<std::int16_t>(place & 0xffffU);
        node.target = readLe32(bytes, at + 12);
        nodes.push_back(node);
    }
    std::stable_sort(nodes.begin(), nodes.end(), [](const PathNode &a, const PathNode &b) {
        if (a.position != b.position) return a.position < b.position;
        return a.line < b.line;
    });
    return nodes;
}

std::vector<SoundEmitter> parseSounds(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) throw std::runtime_error("sounds.nod: no header");
    const auto count = static_cast<std::size_t>(readLe32(bytes, 0));
    if (bytes.size() < 8 + count * 16) throw std::runtime_error("sounds.nod: short file");
    std::vector<SoundEmitter> emitters;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t at = 8 + index * 16;
        SoundEmitter emitter;
        emitter.x = static_cast<std::int32_t>(readLe32(bytes, at));
        emitter.y = static_cast<std::int32_t>(readLe32(bytes, at + 4));
        emitter.type = static_cast<std::int16_t>(readLe32(bytes, at + 8) & 0xffffU);
        if (emitter.type < 0 || emitter.type > 4)
            throw std::runtime_error("sounds.nod: Illegal sound-values!");
        emitters.push_back(emitter);
    }
    return emitters;
}

std::string formatRaceTime(std::uint32_t milliseconds) {
    const std::uint32_t minutes = milliseconds / 1000U / 60U;
    if (minutes >= 100U) return "99'99\"99";
    char text[32];
    std::snprintf(text, sizeof text, "%u'%02u\"%02u", minutes,
                  (milliseconds / 1000U) % 60U, (milliseconds / 10U) % 100U);
    return text;
}

} // namespace fgp::racing
