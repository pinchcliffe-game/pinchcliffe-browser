#include "platform/web_audio.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// clang-format off
EM_JS(int, fgp_suspend_web_audio, (), {
    var SDL2 = Module['SDL2'];
    var context = SDL2 && SDL2.audioContext;
    if (!context || context.state !== 'running') return 0;
    context.suspend();
    return 1;
});
EM_JS(void, fgp_resume_web_audio, (), {
    var SDL2 = Module['SDL2'];
    var context = SDL2 && SDL2.audioContext;
    if (context && context.state !== 'closed') {
        context.resume().catch(function () {});
    }
});
// clang-format on
#endif

namespace fgp {

namespace {
bool webAudioSuspended = false;
} // namespace

void suspendWebAudioForBlockingWork() {
#ifdef __EMSCRIPTEN__
    webAudioSuspended = fgp_suspend_web_audio() != 0;
#endif
}

void resumeWebAudioAfterBlockingWork() {
#ifdef __EMSCRIPTEN__
    if (webAudioSuspended) {
        fgp_resume_web_audio();
    }
#endif
    webAudioSuspended = false;
}

} // namespace fgp
