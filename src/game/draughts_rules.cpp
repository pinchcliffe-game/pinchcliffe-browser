#include "game/draughts_rules.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace fgp {
namespace draughts {
namespace {

// Initial records at 0x00506460 (0xb8 bytes each: type, side, column, row,
// then the pawn descriptor). 0x00464570 copies the header for piece i.
constexpr std::array<std::array<int, 2>, 24> kInitialSquares{{
    {{0, 1}}, {{1, 0}}, {{1, 2}}, {{2, 1}}, {{3, 0}}, {{3, 2}},
    {{4, 1}}, {{5, 0}}, {{5, 2}}, {{6, 1}}, {{7, 0}}, {{7, 2}},
    {{0, 5}}, {{0, 7}}, {{1, 6}}, {{2, 5}}, {{2, 7}}, {{3, 6}},
    {{4, 5}}, {{4, 7}}, {{5, 6}}, {{6, 5}}, {{6, 7}}, {{7, 6}},
}};

// Direction pairs indexed by type + side * 2 (0x0049fd50 for the capture
// test 0x004654f0, the identical 0x0049fdd0 for move generation
// 0x00465790); men use the first two, kings all four.
constexpr std::array<std::array<std::array<int, 2>, 4>, 4> kDirections{{
    {{{{-1, 1}}, {{1, 1}}, {{0, 0}}, {{0, 0}}}},
    {{{{-1, 1}}, {{1, 1}}, {{-1, -1}}, {{1, -1}}}},
    {{{{-1, -1}}, {{1, -1}}, {{0, 0}}, {{0, 0}}}},
    {{{{-1, 1}}, {{1, 1}}, {{-1, -1}}, {{1, -1}}}},
}};

// 0x0049fd00..0x0049fd0c: board origin (188,137), column pitch 53.5 and
// row pitch 54; 0x00464680 then shifts the origin to (193,134).
constexpr int kOriginX = 188;
constexpr int kOriginY = 137;
constexpr float kColumnPitch = 53.5F;
constexpr int kRowPitch = 54;

// 0x0049fe50: FLT_MAX, the "no move" score.
constexpr float kNoMove = FLT_MAX;
// 0x0049fcac / 0x0049c8f8: the repetition penalty.
constexpr float kRepetition = 10000.0F;
// 0x0049fcb0 (0x3d99999a): 0x00460b30's equality tolerance.
constexpr float kTieTolerance = 0.075F;

bool onBoard(int col, int row) {
    return col >= 0 && col <= 7 && row >= 0 && row <= 7;
}

std::size_t square(int col, int row) {
    return static_cast<std::size_t>(col + row * 8);
}

void setSquare(Board &board, int col, int row, int piece) {
    board.squares[square(col, row)] = static_cast<std::int8_t>(piece);
}

// The search state 0x004655a0 builds on its stack and passes to the move
// generator: depth, side to maximise, captures-only and the best move.
struct SearchContext {
    int depth{};
    bool maximizing{};
    bool capturesOnly{};
    Move best;
};

// MSVC's rand() (0x0048d323) on the computer's thread. The thread is new
// for every turn (CreateThread at 0x004651ac) and the CRT initialises a
// thread's seed to 1 (0x00493a57), so every search starts from seed 1.
struct ThreadRandom {
    std::uint32_t state{1};
    int next() {
        state = state * 0x343fdU + 0x269ec3U;
        return static_cast<int>((state >> 16) & 0x7fffU);
    }
};

class Searcher {
public:
    Searcher(Board board, const History &history)
        : board_(board), history_(history) {}

    Board &board() { return board_; }

    // 0x004655a0: best move for one side; depth 0 is the static score.
    float search(int depth, Move *out, int restriction, bool maximizing) {
        const int side = maximizing ? 0 : 1;
        if (depth == 0) return evaluate(board_);
        SearchContext context;
        context.depth = depth;
        context.maximizing = maximizing;
        context.capturesOnly = restriction >= 0;
        context.best = {-1, -1, -1, -1, maximizing ? -kNoMove : kNoMove};
        for (int index = 0; index < 24; ++index) {
            const Piece &piece = board_.pieces[static_cast<std::size_t>(index)];
            if (piece.side != side || !alive(piece)) continue;
            if (restriction >= 0 && restriction != index && restriction != 24) continue;
            generate(context, index);
        }
        if (out) *out = context.best;
        return context.best.score;
    }

private:
    // 0x00465790.
    void generate(SearchContext &context, int index) {
        const Piece piece = board_.pieces[static_cast<std::size_t>(index)];
        const auto &directions = kDirections[static_cast<std::size_t>(
            piece.type + piece.side * 2)];
        const int count = piece.type * 2 + 2;
        for (int k = 0; k < count; ++k) {
            const auto [dc, dr] = directions[static_cast<std::size_t>(k)];
            const int col = piece.col + dc;
            const int row = piece.row + dr;
            if (!onBoard(col, row)) continue;
            const int occupant = pieceAt(board_, col, row);
            if (occupant < 0) {
                if (!context.capturesOnly) {
                    tryMove(context, index, col, row, -1);
                }
                continue;
            }
            if (board_.pieces[static_cast<std::size_t>(occupant)].side ==
                piece.side) {
                continue;
            }
            const int jumpCol = piece.col + dc * 2;
            const int jumpRow = piece.row + dr * 2;
            if (!onBoard(jumpCol, jumpRow) ||
                pieceAt(board_, jumpCol, jumpRow) >= 0) {
                continue;
            }
            tryMove(context, index, jumpCol, jumpRow, occupant);
        }
    }

    // 0x00465880: play the move on the board, score it, keep the best.
    void tryMove(SearchContext &context, int index, int col, int row,
                 int captured) {
        const Board saved = board_;
        const Piece before = board_.pieces[static_cast<std::size_t>(index)];
        Move move{index, col, row, captured, 0.0F};
        applyMove(board_, move);
        move.score = context.maximizing ? -kNoMove : kNoMove;
        if (captured >= 0) {
            // 0x00465906: the same side keeps jumping with this piece, at
            // the same depth; the crowning result is not consulted.
            move.score = search(context.depth, nullptr, index,
                                context.maximizing);
        }
        if (std::fabs(move.score) == kNoMove) {
            const bool opponent = !context.maximizing;
            move.score = search(context.depth - 1, nullptr,
                                sideHasCapture(board_, opponent ? 0 : 1)
                                    ? 24 : -1,
                                opponent);
        }
        if (repeated(before, move)) {
            move.score = move.score +
                         (context.maximizing ? -kRepetition : kRepetition);
        }
        const float best = context.best.score;
        const bool better = context.maximizing ? best < move.score
                                               : best > move.score;
        if (better || tie(best, move.score)) context.best = move;
        board_ = saved;
    }

    // 0x00465a10: the same quiet move by the same piece is in the last 32.
    bool repeated(const Piece &before, const Move &move) const {
        if (move.captured >= 0) return false;
        for (const auto &entry : history_.entries) {
            if (entry.piece < 0 || entry.piece != move.piece ||
                entry.captured >= 0) {
                continue;
            }
            if (entry.toCol == move.col && entry.toRow == move.row &&
                entry.fromCol == before.col && entry.fromRow == before.row) {
                return true;
            }
        }
        return false;
    }

    // 0x00460b30: equal within 0.075 replaces one time in eight.
    bool tie(float best, float candidate) {
        const double low = static_cast<double>(best) - kTieTolerance;
        const double high = static_cast<double>(best) + kTieTolerance;
        if (!(low < candidate) || !(high > candidate)) return false;
        return random_.next() % 8 == 0;
    }

    Board board_;
    const History &history_;
    ThreadRandom random_;
};

} // namespace

Board initialBoard() {
    Board board;
    board.squares.fill(-1);
    for (std::size_t index = 0; index < 24; ++index) {
        auto &piece = board.pieces[index];
        piece.type = 0;
        piece.side = index < 12 ? 0 : 1;
        piece.col = kInitialSquares[index][0];
        piece.row = kInitialSquares[index][1];
        setSquare(board, piece.col, piece.row, static_cast<int>(index));
    }
    return board;
}

bool alive(const Piece &piece) {
    // 0x0045fee0.
    return piece.col >= 0 && piece.row >= 0;
}

int pieceAt(const Board &board, int col, int row) {
    return board.squares[square(col, row)];
}

int pieceRank(const Piece &piece) {
    // 0x00464ed0: rows from the owner's side.
    return piece.side == 0 ? piece.row : 7 - piece.row;
}

int alivePieces(const Board &board, int side) {
    // 0x00464440.
    return static_cast<int>(std::count_if(
        board.pieces.begin(), board.pieces.end(), [side](const Piece &piece) {
            return piece.side == side && alive(piece);
        }));
}

bool sideHasCapture(const Board &board, int side) {
    // 0x00465490 with 0x004654f0 for every live piece of the side.
    for (const Piece &piece : board.pieces) {
        if (piece.side != side || !alive(piece)) continue;
        const auto &directions = kDirections[static_cast<std::size_t>(
            piece.type + piece.side * 2)];
        const int count = piece.type * 2 + 2;
        for (int k = 0; k < count; ++k) {
            const auto [dc, dr] = directions[static_cast<std::size_t>(k)];
            const int col = piece.col + dc;
            const int row = piece.row + dr;
            if (!onBoard(col, row)) continue;
            const int occupant = pieceAt(board, col, row);
            if (occupant < 0 ||
                board.pieces[static_cast<std::size_t>(occupant)].side ==
                    piece.side) {
                continue;
            }
            const int jumpCol = piece.col + dc * 2;
            const int jumpRow = piece.row + dr * 2;
            if (onBoard(jumpCol, jumpRow) &&
                pieceAt(board, jumpCol, jumpRow) < 0) {
                return true;
            }
        }
    }
    return false;
}

bool pieceCanMove(const Board &board, int index, bool capturesOnly) {
    // 0x00465cf0, test for test.
    const Piece &piece = board.pieces[static_cast<std::size_t>(index)];
    const int dir = piece.side != 0 ? -1 : 1;
    const int col = piece.col;
    const int row = piece.row;
    const int rank = pieceRank(piece);
    const auto empty = [&](int c, int r) { return pieceAt(board, c, r) < 0; };
    const auto enemy = [&](int c, int r) {
        const int occupant = pieceAt(board, c, r);
        return occupant >= 0 &&
               board.pieces[static_cast<std::size_t>(occupant)].side !=
                   piece.side;
    };
    if (!capturesOnly) {
        if (col > 0 && rank < 7 && empty(col - 1, row + dir)) return true;
        if (col < 7 && rank < 7 && empty(col + 1, row + dir)) return true;
    }
    if (col > 1 && rank < 6 && empty(col - 2, row + dir * 2) &&
        enemy(col - 1, row + dir)) {
        return true;
    }
    if (col < 6 && rank < 6 && empty(col + 2, row + dir * 2) &&
        enemy(col + 1, row + dir)) {
        return true;
    }
    if (piece.type != 1) return false;
    if (!capturesOnly) {
        if (col > 0 && rank > 0 && empty(col - 1, row - dir)) return true;
        if (col < 7 && rank > 0 && empty(col + 1, row - dir)) return true;
    }
    if (col > 1 && rank > 1 && empty(col - 2, row - dir * 2) &&
        enemy(col - 1, row - dir)) {
        return true;
    }
    return col < 6 && rank > 1 && empty(col + 2, row - dir * 2) &&
           enemy(col + 1, row - dir);
}

int movablePieces(const Board &board, int side) {
    // 0x00465ca0 through 0x00465ce0.
    int count = 0;
    for (int index = 0; index < 24; ++index) {
        const Piece &piece = board.pieces[static_cast<std::size_t>(index)];
        if (piece.side == side && alive(piece) &&
            pieceCanMove(board, index, false)) {
            ++count;
        }
    }
    return count;
}

bool validateMove(const Board &board, int index, int col, int row,
                  int *captured) {
    // 0x00465a80.
    const Piece &piece = board.pieces[static_cast<std::size_t>(index)];
    const int dc = col - piece.col;
    const int dr = row - piece.row;
    if (!onBoard(col, row)) return false;                 // outside board
    if (std::abs(dc) != std::abs(dr)) return false;       // not diagonal
    if (pieceAt(board, col, row) >= 0) return false;      // square not empty
    if (piece.type == 0) {
        // is_backwards 0x00465c50.
        if (piece.side == 0 ? dr < 0 : dr > 0) return false;
    }
    switch (std::abs(dc)) {
    case 1:
        // 0x00465c0b: a plain step is legal only when no capture exists.
        *captured = -1;
        return !sideHasCapture(board, piece.side);
    case 2: {
        const int middle = pieceAt(board, piece.col + dc / 2,
                                   piece.row + dr / 2);
        if (middle < 0) return false;                     // no piece
        if (board.pieces[static_cast<std::size_t>(middle)].side ==
            piece.side) {
            return false;                                 // own piece
        }
        *captured = middle;
        return true;
    }
    default:
        return false;                                     // too far
    }
}

bool applyMove(Board &board, const Move &move) {
    // 0x00464d50 / 0x00464f20 / 0x00464de0 without their visible effects.
    if (move.captured >= 0) {
        Piece &captured = board.pieces[static_cast<std::size_t>(move.captured)];
        setSquare(board, captured.col, captured.row, -1);
        captured.col = -1;
        captured.row = -1;
    }
    Piece &piece = board.pieces[static_cast<std::size_t>(move.piece)];
    setSquare(board, piece.col, piece.row, -1);
    bool notCrowned = true;
    piece.col = move.col;
    piece.row = move.row;
    if (piece.type == 0 && pieceRank(piece) == 7) {
        notCrowned = false;
        piece.type = 1;
    }
    setSquare(board, move.col, move.row, move.piece);
    return notCrowned;
}

void recordHistory(History &history, const Board &board, const Move &move) {
    // 0x00464f60.
    const Piece &piece = board.pieces[static_cast<std::size_t>(move.piece)];
    auto &entry = history.entries[static_cast<std::size_t>(history.next)];
    entry = {move.piece, move.col, move.row, piece.col, piece.row,
             move.captured};
    history.next = (history.next + 1) % 32;
}

float evaluate(const Board &board) {
    // 0x00465680 summing 0x004656c0 over all 24 records.
    float total = 0.0F;
    for (const Piece &piece : board.pieces) {
        if (!alive(piece)) continue;
        const float weight = piece.type == 0 ? 1.0F : 5.0F;
        const int position = piece.side == 0 ? piece.row + 11 : 18 - piece.row;
        float score = static_cast<float>(position) * weight;
        if (piece.side == 1) score = -score;
        total += score;
    }
    return total;
}

std::vector<Move> computeMoves(const Board &board, const History &history,
                               int turn, int depth) {
    // 0x004651c0.
    Searcher searcher(board, history);
    const bool maximizing = turn == 0;
    const int restriction =
        sideHasCapture(searcher.board(), maximizing ? 0 : 1) ? 24 : -1;
    Move move;
    float score = searcher.search(depth, &move, restriction, maximizing);
    if (std::fabs(score) == kNoMove) {
        // 0x0046522e: a decided game is re-searched one ply deep.
        score = searcher.search(1, &move, restriction, maximizing);
    }
    std::vector<Move> moves;
    if (move.piece < 0) return moves;
    applyMove(searcher.board(), move);
    moves.push_back(move);
    while (move.captured >= 0) {
        const int piece = move.piece;
        if (std::fabs(searcher.search(depth, &move, piece, maximizing)) ==
            kNoMove) {
            break;
        }
        const bool notCrowned = applyMove(searcher.board(), move);
        moves.push_back(move);
        if (!notCrowned) break;
    }
    return moves;
}

Point squareOrigin(int col, int row) {
    // 0x004646d0 converts through _ftol 0x0048c214, which truncates
    // (column 7: 562.5 -> 562, so 567 on screen, as captured);
    // 0x00464680 then shifts the origin.
    const float x = static_cast<float>(col) * kColumnPitch +
                    static_cast<float>(kOriginX);
    return {static_cast<int>(x) + (0xc1 - kOriginX),
            row * kRowPitch + kOriginY + (0x86 - kOriginY)};
}

Point squareAt(int x, int y) {
    // 0x00464980: both divisions truncate toward zero.
    return {static_cast<int>(static_cast<float>(x - kOriginX) / kColumnPitch),
            (y - kOriginY) / kRowPitch};
}

} // namespace draughts

} // namespace fgp
