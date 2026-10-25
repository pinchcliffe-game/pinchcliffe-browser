#include "game/chess_engine.h"

#include <cstdlib>

namespace fgp::chess {
namespace {

struct Step {
    int dx;
    int dy;
};
// Direction tables at 0x0049fb30 (bishop), 0x0049fb50 (knight),
// 0x0049fb90 (rook) and 0x0049fbb0/0x0049fbf0 (queen, king).
constexpr Step kBishopSteps[4]{{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
constexpr Step kKnightSteps[8]{{1, 2},   {2, 1},   {2, -1}, {1, -2},
                               {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
constexpr Step kRookSteps[4]{{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
constexpr Step kRoyalSteps[8]{{0, 1},  {1, 1},   {1, 0},  {1, -1},
                              {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};

// 0x0049fc80: a pawn's value by file.
constexpr float kPawnValue[8]{1.0F, 1.1F, 1.2F, 1.3F, 1.3F, 1.2F, 1.1F, 1.0F};
constexpr float kRankWeight = 0.2F;          // 0x0049eb20
constexpr float kOne = 1.0F;                 // 0x0049c758
constexpr float kMateScore = 3.0e38F;        // 0x0049fca4 (0x0049fca0 negated)
constexpr float kRepetitionPenalty = 10000.0F; // 0x0049c8f8 (0x0049fcac negated)
constexpr float kTieWindow = 0.075F;         // 0x0049fcb0

int sign(int value) { return value > 0 ? 1 : (value < 0 ? -1 : 0); }

// 0x00460650.
float pieceValue(const Piece *piece) {
    if (piece == nullptr) return 0.0F;
    switch (piece->type) {
    case Pawn: return kPawnValue[piece->x];
    case Bishop:
    case Knight: return 4.0F;                // 0x0049d9ac
    case Rook: return 5.0F;                  // 0x0049d9fc
    case Queen: return 9.9F;                 // 0x0049fca8
    case King: return kOne;                  // 0x0049c758
    default: return 0.0F;
    }
}

} // namespace

void Engine::reset() {
    // The piece table at 0x00503680: side 0 pawns, side 0 back row, side 1
    // pawns, side 1 back row, each from x = 0 to 7.
    constexpr int kBackRow[8]{Rook, Knight, Bishop, Queen,
                              King, Bishop, Knight, Rook};
    position = Position{};
    for (int index = 0; index < 32; ++index) {
        Piece &piece = position.pieces[static_cast<std::size_t>(index)];
        const int group = index / 8;
        piece.side = group / 2;
        piece.x = index % 8;
        const bool pawns = group % 2 == 0;
        piece.type = pawns ? Pawn : kBackRow[piece.x];
        piece.y = piece.side == 0 ? (pawns ? 1 : 0) : (pawns ? 6 : 7);
        piece.descriptor = index;
        setCell(piece.x, piece.y, index + 1);
    }
    history = {};
    historyIndex = 0;
}

void Engine::setCell(int x, int y, int value) {
    if (x < 0 || x > 7 || y < 0 || y > 7) return;
    position.board[static_cast<std::size_t>(x + y * 8)] = value;
}

int Engine::indexAt(int x, int y) const {
    if (x < 0 || x > 7 || y < 0 || y > 7) return 0;
    return position.board[static_cast<std::size_t>(x + y * 8)];
}

const Piece *Engine::pieceAt(int x, int y) const {
    const int index = indexAt(x, y);
    return index == 0 ? nullptr
                      : &position.pieces[static_cast<std::size_t>(index - 1)];
}

bool Engine::alive(int index) const {
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    return piece.x >= 0 && piece.y >= 0;
}

bool Engine::canCheck(int attacker, int target) const {
    const Piece &piece = position.pieces[static_cast<std::size_t>(attacker)];
    const auto ray = [&](int range, int count, const Step *steps) {
        // 0x00460100.
        for (int direction = 0; direction < count; ++direction) {
            for (int step = 1; step <= range; ++step) {
                const int x = steps[direction].dx * step + piece.x;
                const int y = steps[direction].dy * step + piece.y;
                if (x < 0 || x > 7 || y < 0 || y > 7) break;
                const int occupant = indexAt(x, y);
                if (occupant != 0) {
                    if (occupant == target + 1) return true;
                    break;
                }
            }
        }
        return false;
    };
    switch (piece.type) {
    case Pawn: {
        const int y = forwardBy(piece.side, piece.y, 1);
        if (piece.x > 0 && indexAt(piece.x - 1, y) == target + 1) return true;
        if (piece.x < 7 && indexAt(piece.x + 1, y) == target + 1) return true;
        return false;
    }
    case Bishop: return ray(8, 4, kBishopSteps);
    case Knight: return ray(1, 8, kKnightSteps);
    case Rook: return ray(8, 4, kRookSteps);
    case Queen: return ray(8, 8, kRoyalSteps);
    case King: return ray(1, 8, kRoyalSteps);
    default: return false;
    }
}

bool Engine::isAttacked(int target) const {
    const int side = position.pieces[static_cast<std::size_t>(target)].side;
    for (int index = 0; index < 32; ++index) {
        if (position.pieces[static_cast<std::size_t>(index)].side != side &&
            alive(index) && canCheck(index, target))
            return true;
    }
    return false;
}

bool Engine::isValidMoveShape(int index, int tx, int ty, int captured) const {
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    int x = piece.x;
    int y = piece.y;
    const int dx = tx - x;
    const int dy = ty - y;
    // The sliding loops of 0x00460fb0 step until they reach the target or
    // meet a piece; a zero step would never end, and the callers never ask.
    const auto walk = [&](int sx, int sy) {
        if (sx == 0 && sy == 0) return false;
        do {
            x += sx;
            y += sy;
            if (x == tx && y == ty) return true;
        } while (indexAt(x, y) == 0);
        return false;
    };
    switch (piece.type) {
    case Pawn: {
        const int rank = rankOf(piece.side, y);
        const int one = forwardBy(piece.side, y, 1);
        const int two = forwardBy(piece.side, y, 2);
        if (forwardOf(piece.side, ty, y) && !forwardOf(piece.side, ty, two) &&
            (rank == 1 || !forwardOf(piece.side, ty, one)) &&
            (ty != two || indexAt(tx, one) == 0)) {
            if (captured == 0) return tx == x;
            if (std::abs(dx) == 1) return ty == one;
        }
        return false;
    }
    case Bishop:
        return std::abs(dx) == std::abs(dy) && walk(sign(dx), sign(dy));
    case Knight:
        return (std::abs(dx) == 2 && std::abs(dy) == 1) ||
               (std::abs(dx) == 1 && std::abs(dy) == 2);
    case Rook:
        if (tx == x) return walk(0, sign(dy));
        if (ty == y) return walk(sign(dx), 0);
        return false;
    case Queen:
        if (tx == x || ty == y || std::abs(dx) == std::abs(dy))
            return walk(sign(dx), sign(dy));
        return false;
    case King:
        return std::abs(dx) < 2 && std::abs(dy) < 2;
    default:
        return false;
    }
}

bool Engine::isLegalHumanMove(int index, int tx, int ty, int captured) {
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    if (captured != 0) {
        const Piece &target =
            position.pieces[static_cast<std::size_t>(captured - 1)];
        if (target.side == piece.side) return false;  // "attempt to take own piece"
        if (target.type == King) return false;        // "attempt to take king!"
    }
    if (!isValidMoveShape(index, tx, ty, captured)) return false;
    const int king = kKingIndex[static_cast<std::size_t>(piece.side)];
    const Position saved = position;
    apply(Move{index + 1, tx, ty, captured});
    const bool attacked = isAttacked(king);
    position = saved;
    return !attacked;
}

void Engine::apply(const Move &move) {
    if (move.captured != 0) {
        // 0x004603a0.
        Piece &taken = position.pieces[static_cast<std::size_t>(move.captured - 1)];
        setCell(taken.x, taken.y, 0);
        taken.x = -1;
        taken.y = -1;
    }
    // 0x00460260: a pawn reaching the far rank becomes a queen.
    Piece &piece = position.pieces[static_cast<std::size_t>(move.piece - 1)];
    setCell(piece.x, piece.y, 0);
    piece.x = move.x;
    piece.y = move.y;
    if (piece.type == Pawn && rankOf(piece.side, move.y) == 7) {
        piece.type = Queen;
    }
    setCell(move.x, move.y, move.piece);
}

void Engine::record(const Move &move) {
    const Piece &piece = position.pieces[static_cast<std::size_t>(move.piece - 1)];
    HistoryEntry &entry = history[static_cast<std::size_t>(historyIndex)];
    entry.piece = move.piece;
    entry.x = move.x;
    entry.y = move.y;
    entry.fromX = piece.x;
    entry.fromY = piece.y;
    entry.captured = move.captured;
    historyIndex = (historyIndex + 1) % 6;
}

bool Engine::repeats(const Piece &before, const Move &move) const {
    for (const HistoryEntry &entry : history) {
        if (entry.piece != 0 && entry.piece == move.piece &&
            entry.captured == 0 && move.captured == 0 && entry.x == move.x &&
            entry.y == move.y && entry.fromX == before.x &&
            entry.fromY == before.y)
            return true;
    }
    return false;
}

double Engine::pawnSupport(int index) const {
    // 0x004606d0: the values of whatever stands diagonally in front.
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    const int y = forwardBy(piece.side, piece.y, 1);
    if (y < 0 || y > 7) return 0.0;
    float left = 0.0F;
    if (piece.x - 1 >= 0) left = pieceValue(pieceAt(piece.x - 1, y));
    if (piece.x + 1 < 8)
        return static_cast<double>(pieceValue(pieceAt(piece.x + 1, y))) +
               static_cast<double>(left);
    return static_cast<double>(left);
}

double Engine::pieceScore(int index) const {
    // 0x004605d0, accumulated in the x87's extended format and rounded to
    // float where the original stores it.
    if (!alive(index)) return 0.0;
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    double score =
        (static_cast<double>(rankOf(piece.side, piece.y)) *
             static_cast<double>(kRankWeight) +
         static_cast<double>(kOne)) *
        static_cast<double>(pieceValue(&piece));
    if (piece.type == Pawn) {
        score = pawnSupport(index) +
                static_cast<double>(static_cast<float>(score));
    }
    return piece.side == 1 ? -score : score;
}

float Engine::evaluate() const {
    float sum = 0.0F;
    for (int index = 0; index < 32; ++index) {
        sum = static_cast<float>(pieceScore(index) + static_cast<double>(sum));
    }
    if (!alive(kKingIndex[1])) return kMateScore;
    if (!alive(kKingIndex[0])) return -kMateScore;
    return sum;
}

bool Engine::nearTie(float best, float score, Random &random) const {
    const double window = static_cast<double>(kTieWindow);
    if (static_cast<double>(best) - window < static_cast<double>(score) &&
        static_cast<double>(score) < static_cast<double>(best) + window) {
        return (pinnedRandom >= 0 ? pinnedRandom : random.below(8)) == 0;
    }
    return false;
}

void Engine::tryMove(Node &node, int index, int tx, int ty, int captured,
                     Random &random) {
    const Piece before = position.pieces[static_cast<std::size_t>(index)];
    const int king = kKingIndex[static_cast<std::size_t>(before.side)];
    const Position saved = position;
    Move move{index + 1, tx, ty, captured};
    apply(move);
    if (!isAttacked(king)) {
        float score = search(node.depth - 1, !node.maximizing, random, nullptr);
        if (repeats(before, move)) {
            score = (node.maximizing ? -kRepetitionPenalty
                                     : kRepetitionPenalty) +
                    score;
        }
        const float best = node.best.score;
        const bool better = node.maximizing ? best < score : score < best;
        if (better || nearTie(best, score, random)) {
            move.score = score;
            node.best = move;
        }
    }
    position = saved;
}

void Engine::generate(Node &node, int index, Random &random) {
    const Piece &piece = position.pieces[static_cast<std::size_t>(index)];
    const int side = piece.side;
    const int x = piece.x;
    const int y = piece.y;
    const auto rays = [&](int range, int count, const Step *steps) {
        // 0x00460b70.
        for (int direction = 0; direction < count; ++direction) {
            for (int step = 1; step <= range; ++step) {
                const int tx = steps[direction].dx * step + x;
                const int ty = steps[direction].dy * step + y;
                if (tx < 0 || tx > 7 || ty < 0 || ty > 7) break;
                const int occupant = indexAt(tx, ty);
                if (occupant != 0) {
                    if (position.pieces[static_cast<std::size_t>(occupant - 1)]
                            .side != side)
                        tryMove(node, index, tx, ty, occupant, random);
                    break;
                }
                tryMove(node, index, tx, ty, 0, random);
            }
        }
    };
    switch (piece.type) {
    case Pawn: {
        if (y < 1 || y > 6) return;
        const int one = forwardBy(side, y, 1);
        const int two = forwardBy(side, y, 2);
        if (indexAt(x, one) == 0) tryMove(node, index, x, one, 0, random);
        if (rankOf(side, y) == 1 && indexAt(x, one) == 0 &&
            indexAt(x, two) == 0)
            tryMove(node, index, x, two, 0, random);
        for (const int cx : {x - 1, x + 1}) {
            if (cx < 0 || cx > 7) continue;
            const int occupant = indexAt(cx, one);
            if (occupant != 0 &&
                position.pieces[static_cast<std::size_t>(occupant - 1)].side !=
                    side)
                tryMove(node, index, cx, one, occupant, random);
        }
        return;
    }
    case Bishop: rays(8, 4, kBishopSteps); return;
    case Knight: rays(1, 8, kKnightSteps); return;
    case Rook: rays(8, 4, kRookSteps); return;
    case Queen: rays(8, 8, kRoyalSteps); return;
    case King: rays(1, 8, kRoyalSteps); return;
    default: return;
    }
}

float Engine::search(int plies, bool maximizing, Random &random, Move *best) {
    if (plies == 0) return evaluate();
    Node node;
    node.depth = plies;
    node.maximizing = maximizing;
    node.best.score = maximizing ? -std::numeric_limits<float>::max()
                                 : std::numeric_limits<float>::max();
    const int side = maximizing ? 0 : 1;
    for (int index = 0; index < 32; ++index) {
        if (position.pieces[static_cast<std::size_t>(index)].side == side &&
            alive(index))
            generate(node, index, random);
    }
    if (best != nullptr) *best = node.best;
    return node.best.score;
}

} // namespace fgp::chess
