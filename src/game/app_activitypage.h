#pragma once

#include <array>
#include <memory>

#include "game/activity_panel.h"
#include "game/app_core.h"

namespace fgp {

// The "activity" scene (Aktivitetssenteret): a page the player fills with
// text, shapes and pictures, then prints or saves. Implemented in
// app_activitypage.cpp; the dispatcher is FGP.exe 0x0045be90.
class ActivitypageScene : protected virtual AppCore {
protected:
    // One of the 32 object slots at 0x00698730 (0x108 bytes each).
    struct ActivityObject {
        bool used{};
        // +4: the tab that made it, 10 text, 11 shape, 12 picture.
        int type{};
        // +8: SCENE layer, from the counter at 0x005028b8.
        int layer{};
        // +0xc: the object's descriptor.
        int sprite{-1};
        // Text (+0xb4 font, +0xb8 size, +0xbc colour, +0xc0 style bits
        // 1 bold / 2 italic / 4 underline, +0xc4 64 characters).
        int font{};
        int size{};
        std::uint32_t colour{};
        unsigned style{};
        std::string text;
        // Shape (+0xb4 shape, +0xb8 colour, +0xbc width, +0xc0 height).
        int shape{};
        int width{};
        int height{};
        // Picture (+0xb4 library index or 0x80, +0xb8 bitmap, +0xbc/+0xc0
        // horizontal and vertical scale).
        int picture{};
        SurfacePtr bitmap;
        double scaleX{};
        double scaleY{};
    };
    struct ActivityFont {
        std::string name;
        BitmapFont font;
    };
    // The spinner at 0x0069a830.
    struct ActivitySpinner {
        bool dragging{};
        int startX{};
        int startY{};
        int minimum{8};
        int maximum{0x48};
        int step{2};
        int divisor{6};
        int delta{};
        int value{};
    };
    enum class ActivityDrag { None, Object, Resize, Scroll };
    // The modal text box 0x0041ffb0 (common/dialogue/name).
    enum class ActivityTextPurpose { None, NewText, EditText, Save };
    struct ActivitypageGame {
        std::unique_ptr<ActivityStage> stage;
        std::unique_ptr<ActivityPanel> panel;
        std::unique_ptr<ActivityList> list;
        std::unique_ptr<ActivityScrollbar> scrollbar;
        std::unique_ptr<ActivityTooltip> tooltip;
        int logo{-1};
        int title{-1};
        std::array<int, 5> frame{{-1, -1, -1, -1, -1}};
        int paper{-1};
        int swatch{-1};
        int spinnerSprite{-1};
        int selection{-1};
        int closeHandle{-1};
        int resizeHandle{-1};
        std::array<ActivityObject, 32> objects{};
        // 0x0069ab18 tab, 0x0069ab1c colour (COLORREF), 0x00698528
        // selection, 0x005028b8 layer counter.
        int tab{};
        std::uint32_t colour{};
        int selected{-1};
        int layerCounter{-400};
        ActivitySpinner spinner;
        std::vector<ActivityFont> fonts;
        // 0x0069ab20/0x0069ddf8: the picture list's library indexes.
        std::vector<int> pictures;
        // The entry parameter block (0x0069dff8): source scene, mode and
        // the picture it handed over (0x80 in the picture list).
        std::string entryScene;
        int entryMode{};
        SurfacePtr entryPicture;
        std::uint32_t hotspot{};
        bool leftDown{};
        ActivityDrag drag{ActivityDrag::None};
        int dragObject{-1};
        int dragSprite{-1};
        int dragLastX{};
        int dragLastY{};
        ActivityTextPurpose textPurpose{ActivityTextPurpose::None};
        int textObject{-1};
        std::string textValue;
        std::string textTitle;
        std::size_t textLimit{};
        bool announced{};
    };

    ActivitypageGame loadActivitypageGame();
    // Every SDL event while the scene runs; true when consumed.
    bool handleActivitypageEvent(const SDL_Event &event);
    void updateActivitypage(double elapsed);
    void renderActivitypage(double elapsed);
    // Messages confirmed with Yes (interfaceh:WARNING boxes of type 0xc).
    void confirmActivitypageClear();
    void confirmActivitypagePrint();
    // Developer control API for "activity:..." commands. Returns false to
    // reject the command, as GameApp::executeDeveloperCommand does.
    bool executeActivitypageDeveloperCommand(std::string_view command);

    std::optional<ActivitypageGame> activitypageGame;

private:
    void announceActivitypageEntry();
    SurfacePtr activityBitmap(std::string_view name);
    SurfacePtr activityCommonBitmap(std::string_view path);
    std::uint32_t activityHotspotAt(int x, int y);
    void activityHotspotChanged(std::uint32_t hotspot);
    void activityMouseDown(int x, int y, bool doubleClick);
    void activityMouseUp(int x, int y);
    void activityMouseMove(int x, int y);
    void activityRightDown(int x, int y);
    void activityKey(SDL_Keycode key);
    void activityControlClicked(std::uint32_t id);
    void setActivityTab(int tab);
    void setActivitySpinnerRange(int minimum, int maximum);
    void setActivitySpinner(int value);
    void drawActivitySpinner(int value);
    void setActivityColour(std::uint32_t colour);
    void setActivityLayout(int layout);
    int activityLayout() const;
    SDL_Rect activityPageRect() const;
    int freeActivitySlot() const;
    void raiseActivityObject(int slot);
    void addActivityObject();
    bool addActivityText();
    bool addActivityShape();
    bool addActivityPicture();
    bool addActivityPictureObject(int picture);
    int renderActivityText(ActivityObject &object);
    void renderActivityShape(ActivityObject &object);
    void loadActivityPicture(ActivityObject &object);
    void renderActivityPicture(ActivityObject &object);
    void removeActivityObject(int slot);
    void removeSelectedActivityObject();
    void clearActivityObjects();
    void deselectActivityObject();
    void refreshActivitySelection();
    void followActivitySelection();
    void selectActivityObject(int slot);
    int activityObjectAt(int x, int y) const;
    void syncActivitySpinner(int slot);
    void applyActivitySize(int width, int height);
    void applyActivityColour(std::uint32_t colour);
    void activityListPicked(int index);
    unsigned activityStyle() const;
    void openActivityText(ActivityTextPurpose purpose, int slot);
    void closeActivityText(bool accept);
    void renderActivityTextDialog();
    SurfacePtr renderActivityPage() const;
};

} // namespace fgp
