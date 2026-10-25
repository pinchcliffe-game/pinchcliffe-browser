#pragma once

// The browser build's audio output around work that blocks the main thread.

namespace fgp {

// SDL2's Emscripten audio driver fills a ScriptProcessorNode from its
// onaudioprocess handler, which runs on the main thread. While the main
// thread is busy (a scene load decodes every clip synchronously) the node is
// not fed and the browser keeps playing its last buffer, a stuck repeating
// tail. Suspending the AudioContext first stops the output instead.
// resumeWebAudioAfterBlockingWork resumes it only if this suspended it. Both
// do nothing outside the browser build, where SDL feeds audio from its own
// thread.
void suspendWebAudioForBlockingWork();
void resumeWebAudioAfterBlockingWork();

} // namespace fgp
