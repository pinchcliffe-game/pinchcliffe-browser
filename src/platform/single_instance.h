#pragma once

namespace fgp {

// The newest launch of this exact executable survives; older copies of the
// same binary are asked to close and then terminated. Prevents stale test
// windows from surviving rebuilds. Implemented for macOS, Windows and Linux.
void closeOlderNativeInstances();

} // namespace fgp
