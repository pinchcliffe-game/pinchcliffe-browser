#include "platform/web_ui.h"

#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// clang-format off
EM_JS(void, fgp_text_entry, (int wanted, int x, int y, int width, int height), {
    if (Module.fgpTextEntry) {
        Module.fgpTextEntry(!!wanted, { x: x, y: y, width: width, height: height });
    }
});
EM_JS(void, fgp_touch_controls, (const char *spec), {
    if (Module.fgpTouchControls) Module.fgpTouchControls(UTF8ToString(spec));
});
// clang-format on
#endif

namespace {
bool webBackRequested = false;
} // namespace

#ifdef __EMSCRIPTEN__
// The page's Back button (tools/web/shell.html) calls Module._fgp_back_pressed.
extern "C" EMSCRIPTEN_KEEPALIVE void fgp_back_pressed() {
    webBackRequested = true;
}
#endif

namespace fgp {

bool takeWebBackRequest() {
    const bool requested = webBackRequested;
    webBackRequested = false;
    return requested;
}

void setTextEntryWanted(bool wanted, WebRect field) {
#ifdef __EMSCRIPTEN__
    static bool sent = false;
    static bool lastWanted = false;
    static WebRect lastField;
    if (!sent || wanted != lastWanted ||
        (wanted && (field.x != lastField.x || field.y != lastField.y ||
                    field.width != lastField.width ||
                    field.height != lastField.height))) {
        sent = true;
        lastWanted = wanted;
        lastField = field;
        fgp_text_entry(wanted ? 1 : 0, field.x, field.y, field.width,
                       field.height);
    }
#else
    (void)wanted;
    (void)field;
#endif
}

void setTouchControls(std::string_view spec) {
#ifdef __EMSCRIPTEN__
    static std::string last = "\x01";
    if (spec != last) {
        last.assign(spec);
        fgp_touch_controls(last.c_str());
    }
#else
    (void)spec;
#endif
}

} // namespace fgp
