#include <SDL.h>

#include <exception>
#include <iostream>
#include <memory>

#include "game/app.h"
#include "platform/command_line.h"
#include "platform/console.h"
#include "platform/single_instance.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

namespace {

// A page cannot block in a loop of its own, so the browser build hands the
// main loop to the browser and runs one GameApp::step() per animation frame.
std::unique_ptr<fgp::GameApp> webApp;

void reportFatal(const std::exception &error) {
    std::cerr << "FGP Native POC error: " << error.what() << '\n';
}

void webFrame() {
    try {
        if (!webApp->step()) {
            emscripten_cancel_main_loop();
            webApp->shutdown();
            webApp.reset();
        }
    } catch (const std::exception &error) {
        emscripten_cancel_main_loop();
        reportFatal(error);
    }
}

} // namespace
#endif

int main(int argc, char **argv) {
    try {
        fgp::closeOlderNativeInstances();
        // Static: the browser build keeps using the arguments after main()
        // has returned.
        static fgp::Utf8Arguments arguments(argc, argv);
#ifdef __EMSCRIPTEN__
        webApp = std::make_unique<fgp::GameApp>(arguments.argc(),
                                                arguments.argv());
        // 0 = run at the display's refresh rate via requestAnimationFrame.
        emscripten_set_main_loop(webFrame, 0, false);
        return 0;
#else
        fgp::GameApp app(arguments.argc(), arguments.argv());
        return app.run();
#endif
    } catch (const std::exception &error) {
    std::cerr << "FGP Native POC error: " << error.what() << '\n';
    const bool sdlStarted = SDL_WasInit(0) != 0;
    // SDL shows a message box before SDL_Init too, so an error a user would
    // otherwise never see (no data found, from a double-click) still says so.
    if (fgp::fatalErrorNeedsDialog(sdlStarted)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FGP Native POC",
                                 error.what(), nullptr);
    }
    if (sdlStarted) {
        SDL_Quit();
    }
    return 1;
    }
}
