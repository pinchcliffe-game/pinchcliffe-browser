#pragma once

namespace fgp {

// Development mode prints a running commentary of what it recovers from the
// original data. When the application is started from a launcher, or when its
// diagnostics would otherwise be interleaved with an editor's own output, that
// commentary is easier to follow in a window of its own.
//
// Gives the process a dedicated console window and rebinds the standard
// streams to it. Returns true when a console was created.
//
// Output that is already redirected to a file or pipe is left alone unless
// `force` is set, so scripted runs that capture stdout keep working.
bool openDeveloperConsole(bool force);

// Whether a fatal error should also be shown in a message box. Never when
// stderr goes to a file or pipe: a script is reading it, and a modal box would
// hang the run. Otherwise always once SDL is running, and before that only when
// nobody would see stderr: no console at all (a Windows GUI build), a console
// Windows made for this process alone and closes with it (a double-click in
// Explorer), or no terminal (an app bundle opened from the Finder).
bool fatalErrorNeedsDialog(bool sdlStarted);

} // namespace fgp
