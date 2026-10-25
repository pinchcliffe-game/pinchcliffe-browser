#pragma once

#include "game/app_viewer.h"

namespace fgp {

// The "anaglyph" activity (3D-titter): red/cyan 3D scenes the player
// unlocks by finding 3D glasses in the story, plus the Rubberball game.
// Implemented in app_anaglyph.cpp; the scene callback is FGP.exe
// 0x0047c630.
class AnaglyphScene : protected virtual ViewerSceneSupport {
protected:
    // One static descriptor the viewer adds (master list 0x00510a40, the
    // music list 0x00510b6c, and the two frame bitmaps).
    struct AnaglyphAnim {
        bool loaded{};
        bool added{};
        bool playing{};
        int x{};
        int y{};
        int layer{};
        // Media time at which a stopped clip holds, and the clock a
        // running one started from (media time 0 at startedAt).
        double heldTime{};
        double startedAt{};
        double rate{1.0};
        DecodedMedia media;
        TexturePtr texture;
        SurfacePtr bitmap;
        LiveAudioMixer::EffectHandle sound{
            LiveAudioMixer::kInvalidEffectHandle};
        double nextLoopAt{};
        // The order SCENE_AddAnim appended it in, for equal layers.
        std::uint64_t addOrder{};
    };
    // The engine list box (0x004085a0 family) the viewer and the picker use.
    struct AnaglyphList {
        SDL_Rect bounds{};
        std::vector<std::string> items;
        std::vector<bool> grey;
        int selected{};
        int top{};
        int rowHeight{1};
        int visibleRows{1};
    };
    // The shared list dialog 0x00420430 ("listdlg"), modal.
    struct AnaglyphPicker {
        AnaglyphList list;
        SurfacePtr backdrop;
        TexturePtr backdropTexture;
        ViewerWidgets widgets;
        std::string title;
        // The dialog's scroll bar (0x004205f0): up, down and thumb.
        std::array<SurfacePtr, 3> scroll;
        std::array<TexturePtr, 3> scrollTextures;
    };
    struct AnaglyphViewer {
        ViewerWidgets widgets;
        // Amerigo 12 and its grey cut (0x006d48b0, 0x006d2768) for the picker.
        BitmapFont pickerFont;
        BitmapFont pickerGreyFont;
        // The list's scroll bar 0x0c (0x0047c95c-0x0047c9b0): hup, hdown,
        // scrolltab.
        std::array<SurfacePtr, 3> scroll;
        std::array<TexturePtr, 3> scrollTextures;
        std::vector<AnaglyphAnim> anims;
        AnaglyphList list;
        std::optional<AnaglyphPicker> picker;
        bool entered{};
        // The initial picker holds back scene_ready until it closes.
        bool sceneReadyPending{};
        // 0x006d6f94: the loaded scene record, as an index into 0x0050d3b0.
        int scene{-1};
        // 0x006d6f84: the selected list item (chapter or game mode).
        int item{};
        // 0x006d6f80: set when the running chapter has ended.
        bool chapterEnded{};
        // 0x006d26bc: the Smacker frame rate 0x0047cf20 imposes (15..30).
        int frameRate{};
        std::string labelText;
        std::string valueText;
        std::uint64_t nextAddOrder{};
        // Rubberball (scene 0x10): 0x006d6f7c hits, 0x006d6f90 the hits the
        // watched game lasts, 0x006d26ac/0x006d4880 the clicks on START
        // since the last ball and the time of the first, 0x006d6f8c the
        // games played, and the two no-repeat bags 0x006d6f88/0x006d6f78.
        int hits{};
        int watchedTarget{};
        int startClicks{};
        double firstClickAt{};
        int gamesPlayed{};
        // 0x006d4888: the row 0x0041ee50 returned for the last game played.
        std::optional<ScoreRow> lastScoreRow;
        NoRepeatState idleBag;
        NoRepeatState hitBag;
    };

    AnaglyphViewer loadAnaglyphViewer();
    void updateAnaglyph(double elapsed);
    void renderAnaglyph(double elapsed);
    bool beginAnaglyphInteraction(int x, int y);
    bool endAnaglyphInteraction(int x, int y);
    bool handleAnaglyphKey(SDL_Keycode key);
    // Event 0x104 (0x0047d950).
    void leaveAnaglyphScene();
    bool executeAnaglyphDeveloperCommand(std::string_view command);

    std::optional<AnaglyphViewer> anaglyphViewer;

private:
    void ensureAnaglyphAnim(std::size_t index);
    void addAnaglyphAnim(std::size_t index, int x, int y, int layer);
    void removeAnaglyphAnim(std::size_t index);
    void startAnaglyphAnim(std::size_t index, double now);
    void resumeAnaglyphAnim(std::size_t index, double now);
    void stopAnaglyphAnim(std::size_t index, double now);
    void playAnaglyphAnim(std::size_t index, double now);
    void removeAnaglyphList(std::span<const std::size_t> list);
    double anaglyphMediaTime(const AnaglyphAnim &anim, double now) const;
    double anaglyphCompletion(const AnaglyphAnim &anim) const;
    void loadAnaglyphScene(int scene, double now);
    void selectAnaglyphItem(int item, double now);
    void stopAnaglyphScene(double now);
    void playAnaglyphScene(double now);
    void playAnaglyphSequence(std::span<const std::size_t> list, double now);
    void setAnaglyphFrameRate(int frameRate, double now);
    void anaglyphFinished(std::size_t index, double now);
    void anaglyphSequenceFinished(std::span<const std::size_t> list,
                                  std::size_t index, double now);
    void rubberballFinished(std::size_t index, double now);
    std::size_t rubberballNext(int state, std::optional<int> direction);
    void openAnaglyphPicker();
    void closeAnaglyphPicker(int result, double now);
    void anaglyphControl(int id, double now);
    bool anaglyphSceneOpen(int scene);
    void refreshAnaglyphValue();
};

} // namespace fgp
