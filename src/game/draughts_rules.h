#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace fgp {

// The rules engine of the "draughts" activity (FGP.exe module
// 0x00463f60-0x00466250), free of rendering so tests and the computer's
// search can run it directly. Implemented in draughts_rules.cpp.
namespace draughts {

struct Point {
    int x{};
    int y{};
};

// One 0x14-byte board record at 0x006a8d58: type (0 man, 1 king), side,
// column and row (-1 once captured). Side 0 starts on rows 0-2 with the
// w_pawn/w_king art, which is the dark piece on screen; side 1 is the light
// one and moves first. The fifth field, the descriptor, lives in
// DraughtsScene::DraughtsPieceArt.
struct Piece {
    int type{};
    int side{};
    int col{-1};
    int row{-1};
};

// The 24 records plus the 8x8 square table at 0x006a8f38 (piece index or
// -1); 0x00464350 clears both, 0xb8 dwords, and the search copies them.
struct Board {
    std::array<Piece, 24> pieces{};
    std::array<std::int8_t, 64> squares{};
};

// A five-dword move: piece, target column and row, captured piece (-1 for
// none) and the score the search gave it.
struct Move {
    int piece{-1};
    int col{-1};
    int row{-1};
    int captured{-1};
    float score{};
};

// One entry of the 32-move history ring at 0x006a7a18 (0x18 bytes):
// piece, target, origin and capture. 0x00464f60 writes it for every move
// really played; the search reads it to punish repetitions.
struct HistoryEntry {
    int piece{-1};
    int toCol{};
    int toRow{};
    int fromCol{};
    int fromRow{};
    int captured{-1};
};
struct History {
    std::array<HistoryEntry, 32> entries{};
    int next{};
};

Board initialBoard();
bool alive(const Piece &piece);
int pieceAt(const Board &board, int col, int row);
int pieceRank(const Piece &piece);
int alivePieces(const Board &board, int side);
bool sideHasCapture(const Board &board, int side);
bool pieceCanMove(const Board &board, int piece, bool capturesOnly);
int movablePieces(const Board &board, int side);
// 0x00465a80: the player's drop. Returns false for an illegal target; on
// success *captured is the jumped piece or -1.
bool validateMove(const Board &board, int piece, int col, int row,
                  int *captured);
// The board half of 0x00464d50: remove the captured piece, move, promote on
// the far rank. Returns false when the move crowned a man.
bool applyMove(Board &board, const Move &move);
void recordHistory(History &history, const Board &board, const Move &move);
float evaluate(const Board &board);

// The computer's whole turn (thread 0x004651c0): every move it will play,
// in order, for the side `turn` at search depth `depth`.
std::vector<Move> computeMoves(const Board &board, const History &history,
                               int turn, int depth);

// Piece top-left for a square (0x00464680) and the square under a point
// (0x00464980).
Point squareOrigin(int col, int row);
Point squareAt(int x, int y);

} // namespace draughts

} // namespace fgp
