#pragma once

#include "game/app_viewer.h"

namespace fgp {

// The "movieplayer" activity (the TV station's projector): film and sound
// reels the player has found, played on a small screen. Implemented in
// app_movieplayer.cpp; the scene callback is FGP.exe 0x0046d480.
class MovieplayerScene : protected virtual ViewerSceneSupport {
protected:
    // A descriptor the scene adds: film1, film2, the needles and the reel.
    struct MovieplayerClip {
        std::string name;
        SDL_Point position{};
        DecodedMedia media;
        TexturePtr texture;
        double duration{};
        bool added{};
        bool playing{};
        bool loop{};
        // Smacker header flag 2 (Y-interlaced): each decoded line lands on
        // every other row of a frame twice as tall, the rows between stay
        // black. The film reels use it.
        bool interlaced{};
        double startedAt{};
        // When a looping clip's sound is next restarted.
        double nextLoopAt{};
        // The frame a stopped clip holds.
        std::size_t heldFrame{};
        LiveAudioMixer::EffectHandle sound{
            LiveAudioMixer::kInvalidEffectHandle};
    };
    struct MovieplayerViewer {
        ViewerWidgets widgets;
        bool entered{};
        // The hidden car part is still there (scene record key 0 is zero).
        bool carPartShown{};
        // 0x006bfbe0 / 0x006bfbe4: the selected film and sound reel, as
        // positions in 0x0049ff64 / 0x0049ff88; restored from and saved to
        // the scene record (type 0x402, key 1).
        int filmIndex{};
        int soundIndex{};
        int tab{6};
        MovieplayerClip film1;
        MovieplayerClip film2;
        std::array<MovieplayerClip, 3> needles;
        MovieplayerClip reel;
        // EVENT_AddTimer(now + 10000, 20000, 0) from 0x0046d6c4.
        double nextNeedleAt{};
    };

    MovieplayerViewer loadMovieplayerViewer();
    void updateMovieplayer(double elapsed);
    void renderMovieplayer(double elapsed);
    bool beginMovieplayerInteraction(int x, int y);
    bool endMovieplayerInteraction(int x, int y);
    // Event 0x104 (0x0046da20): the selections go back into the profile.
    void leaveMovieplayerScene();
    bool executeMovieplayerDeveloperCommand(std::string_view command);

    std::optional<MovieplayerViewer> movieplayerViewer;

private:
    void loadMovieplayerClip(MovieplayerClip &clip, std::string name,
                             SDL_Point position, bool keyed);
    void addMovieplayerClip(MovieplayerClip &clip, int x, int y);
    void startMovieplayerClip(MovieplayerClip &clip, double now);
    void stopMovieplayerClip(MovieplayerClip &clip, double now);
    void removeMovieplayerClip(MovieplayerClip &clip);
    void playMovieplayerAnimation(MovieplayerClip &clip, double now);
    void selectMovieplayerTab(int tab, double now);
    void prepareMovieplayerReel(double now);
    void selectMovieplayerReel(int award, double now);
    void stopMovieplayerReel(double now);
    void playMovieplayerReel(double now);
    void movieplayerFinished(MovieplayerClip &clip, double now);
    void movieplayerControl(int id, double now);
    std::uint32_t movieplayerFilmAwards();
};

} // namespace fgp
