#pragma once
#include "app_types.h"
#include "localization.h"
#include <d2d1.h>
#include <memory>
#include <utility>

namespace reel {
// How a page replaces the previous one. DrillIn/DrillOut keep the plate still
// and move only the orbit contents; Swap is a same-level page change. Reflow is the
// same page in a new order: each item travels from where it is to its new slot, and an
// item that is gone shrinks away.
enum class PageTransition { None, DrillIn, DrillOut, Swap, Reflow };
// Commit keeps the chosen item for one beat while the rest of the reel leaves.
enum class HideStyle { Cancel, Commit };
// How a drag ends. Cancel returns every item home at once; Drop and Remove leave the items
// where they are for the Reflow page that follows, and Remove drops the lifted one into the trash.
enum class DragEnd { Cancel, Drop, Remove };
inline const wchar_t* TransitionName(PageTransition transition) {
    switch (transition) {
    case PageTransition::DrillIn: return L"drill-in";
    case PageTransition::DrillOut: return L"drill-out";
    case PageTransition::Swap: return L"swap";
    case PageTransition::Reflow: return L"reflow";
    default: return L"none";
    }
}

struct PageItem {
    AppTarget visual;
    bool enabled = true;   // disabled items keep their slot, draw at reduced opacity and cannot be chosen
};

struct PageModel {
    std::vector<PageItem> items;
    std::wstring title;          // diagnostics only; never drawn
    bool isSubmenu = false;      // idle center shows the back chevron
    bool adjustControl = false; // slot 0 has flanking chevrons: the EN/ZH tile on the hub
    bool adjustEarlier = true, adjustLater = true; // a side that cannot move draws at the disabled opacity and never shows the active color
    int preferredIndex = -1;
    int selection = -1;
    bool awaitCenter = false;
    // Reflow only: the previous page's slot of each item, or -1 for a new item. When empty,
    // items are matched to the previous page by app.
    std::vector<int> from;
};

// All calls and lifetime belong to the HWND's UI thread.
class OrbitRenderer {
public:
    explicit OrbitRenderer(HWND window);
    ~OrbitRenderer();
    OrbitRenderer(const OrbitRenderer&) = delete;
    OrbitRenderer& operator=(const OrbitRenderer&) = delete;
    bool Initialize(std::wstring& error);
    void Resize(UINT dpi);
    // Idempotent: unchanged appearance does not rebuild the material or redraw.
    void SetAppearance(const Appearance& appearance);
    // Rebuilds text formats with the language's locale and CJK fallback.
    void SetLanguage(Language language);
    // The percentage the Reel size tile shows; the size itself arrives through Resize.
    void SetReelSize(unsigned percent);
    // Replaces the page. originIndex is the slot in the previous page whose
    // tile triggered a DrillIn (its icon travels to the center), or -1.
    void SetPage(const PageModel& page, PageTransition transition = PageTransition::None, int originIndex = -1);
    // Local canvas DIP hit rectangles beside the adjustable tile at the top orbit:
    // first the left chevron (earlier), then the right one (later).
    std::pair<D2D1_RECT_F, D2D1_RECT_F> AdjustArrowBounds() const;
    void SetAwaitCenter(bool awaitCenter);
    void SetSelection(int index);
    // Dragging on Manage apps. BeginDrag lifts item `index` and shows the trash in the center,
    // drawn disabled when nothing may be removed (`trash` false). DragTo moves the lifted item to
    // `point` (local canvas DIP) and opens its place at `slot` for the others; `overTrash` arms
    // the trash and shrinks the item beneath it. EndDrag hides the trash and restores the center.
    void BeginDrag(int index, bool trash);
    void DragTo(D2D1_POINT_2F point, int slot, bool overTrash);
    void EndDrag(DragEnd end);
    // A tile is lifted: false once a page change, rebuild or hide has put it back.
    bool Dragging() const;
    void Show();
    // Starts a compositor-only exit and returns the milliseconds until Hide may detach.
    unsigned BeginHide(HideStyle style = HideStyle::Cancel);
    void Hide();
    bool UsesBackdrop() const;
    std::wstring Diagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
