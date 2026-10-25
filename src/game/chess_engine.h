#pragma once

// The rules and the computer player of the original Chess activity, ported
// from FGP.exe 0x0045ee90-0x004615ff. Everything here is pure state: the
// scene (app_chess.cpp) owns descriptors, sounds and input. Each function
// names the original routine it ports; the globals are noted where the
// original keeps them.

#include <array>
#include <cstdint>
#include <limits>

namespace fgp::chess {

// Piece types as the original numbers them (0x00460fb0's switch).
enum PieceType : int {
    Pawn = 0, Bishop = 1, Knight = 2, Rook = 3, Queen = 4, King = 5,
};

// One 0x14-byte piece record of the array at 0x006a3ab0. Side 0 uses the
// w_* bitmaps (the dark set at the top); side 1 uses b_* (the light set at
// the bottom), which moves first.
struct Piece {
    int type{};
    int side{};
    int x{-1};
    int y{-1};
    // Index of the animation descriptor (+0x10): 0..31 the piece table at
    // 0x00503690 (stride 0xb8), 32..47 the queen descriptors at 0x00504d80.
    int descriptor{};
};

// The 0x380-byte block the original copies as a whole (0xe0 dwords from
// 0x006a3ab0): 32 pieces, then the 8x8 board at 0x006a3d30. A board cell
// holds the piece index + 1, or 0 when empty (the original stores pointers).
struct Position {
    std::array<Piece, 32> pieces{};
    std::array<int, 64> board{};
};

// A move record {piece, x, y, captured, score} as in 0x006a29b0 and
// 0x006a3e68; piece and captured are index + 1, 0 for none.
struct Move {
    int piece{};
    int x{-1};
    int y{-1};
    int captured{};
    float score{};
};

// One of the six history rows at 0x006a3e80 (0x18 bytes) written by
// 0x004603e0 for every move made on screen.
struct HistoryEntry {
    int piece{};
    int x{};
    int y{};
    int fromX{};
    int fromY{};
    int captured{};
};

// The C runtime rand() the original links (0x0048d320): one state per
// thread, seeded 1 when a thread first calls it.
struct Random {
    std::uint32_t state{1};
    int next() {
        state = state * 214013U + 2531011U;
        return static_cast<int>((state >> 16) & 0x7fffU);
    }
    // 0x0040c530: rand() % n, without calling rand() for n == 0.
    int below(int bound) { return bound != 0 ? next() % bound : 0; }
};

// 0x0049fc48: the king's index for each side.
constexpr std::array<int, 2> kKingIndex{12, 28};

class Engine {
public:
    Position position;
    std::array<HistoryEntry, 6> history{};
    int historyIndex{};            // 0x006a29c4
    // Ply depth, 0x005058d0: level + 2 (Easy 2, Medium 3, Hard 4).
    int depth{3};
    // Set by a developer command: 0x0040c530 then returns this value.
    int pinnedRandom{-1};

    // 0x0045f5c0 for every piece: the start position, from the static
    // table at 0x00503680 (type, side, x, y, then the descriptor).
    void reset();

    const Piece *pieceAt(int x, int y) const;          // 0x0045f960
    int indexAt(int x, int y) const;
    bool alive(int index) const;                       // 0x0045fee0
    bool isAttacked(int target) const;                 // 0x0045ff00
    bool canCheck(int attacker, int target) const;     // 0x0045ff50

    // 0x00460fb0: the piece's own movement rule, ignoring checks.
    bool isValidMoveShape(int piece, int x, int y, int captured) const;
    // 0x00460e80: the human's move test. Returns false for an own piece or
    // a king on the target, an impossible shape, or a move that leaves the
    // mover's king attacked.
    bool isLegalHumanMove(int piece, int x, int y, int captured);

    // 0x004601a0 without its visual side: remove the captured piece
    // (0x004603a0), move and promote (0x00460260).
    void apply(const Move &move);
    // 0x004603e0: append a move made on screen to the six-row history.
    void record(const Move &move);
    // 0x00460ac0: the same quiet move by the same piece from the same square
    // is among the last six moves made on screen.
    bool repeats(const Piece &before, const Move &move) const;

    // 0x00460490: depth-first minimax with no pruning. maximizing selects
    // side 0 (0x006a407c) to move; otherwise side 1 (0x005058d4). Returns
    // the best score and fills best with the chosen move (piece 0 if the
    // side has no legal move). rand() is consumed for near ties.
    float search(int plies, bool maximizing, Random &random, Move *best);
    // 0x00460540: the static evaluation, positive for side 0.
    float evaluate() const;

private:
    struct Node {
        int depth{};
        bool maximizing{};
        Move best{};
    };
    double pieceScore(int index) const;                // 0x004605d0
    double pawnSupport(int index) const;               // 0x004606d0
    // 0x00460750, with the sliding generator 0x00460b70.
    void generate(Node &node, int index, Random &random);
    void tryMove(Node &node, int index, int x, int y, int captured,
                 Random &random);                      // 0x00460980
    bool nearTie(float best, float score, Random &random) const; // 0x00460b30
    void setCell(int x, int y, int value);             // 0x0045f670
};

// 0x004600b0: y advanced by n squares in the side's direction.
constexpr int forwardBy(int side, int y, int n) {
    return side == 0 ? y + n : y - n;
}
// 0x00460360: the rank counted from the side's own back row.
constexpr int rankOf(int side, int y) { return side == 0 ? y : 7 - y; }
// 0x00461300: whether y2 lies in front of y for the side.
constexpr bool forwardOf(int side, int y2, int y) {
    return side == 0 ? y < y2 : y2 < y;
}

} // namespace fgp::chess
