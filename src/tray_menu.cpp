#include "tray_menu.h"
#include "vi_tokens.h"
#include <shellscalingapi.h>
#include <algorithm>
#include <vector>

namespace reel {
namespace {
// uxtheme exports these by ordinal only (Windows 10 1903 and later). They are what makes
// Explorer's and Notepad's popup menus dark; without them a menu stays light on a dark desktop.
enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

COLORREF ToColorRef(const Rgba& color) {
    auto channel = [](float value) { return static_cast<BYTE>(std::clamp(value, 0.f, 1.f) * 255.f + .5f); };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

// The first icon font installed with the code point, selected into `dc`; nullptr when none has it.
HFONT SelectIconFont(HDC dc, wchar_t glyph, int size) {
    for (const wchar_t* family : {vi::kIconFamily, vi::kIconFallbackFamily}) {
        HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, family);
        if (!font) continue;
        SelectObject(dc, font);
        // GDI substitutes another face for a missing family; that face's glyph would be wrong.
        wchar_t face[LF_FACESIZE]{};
        WORD index = 0xFFFF;
        GetTextFaceW(dc, LF_FACESIZE, face);
        if (!_wcsicmp(face, family) && GetGlyphIndicesW(dc, &glyph, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == 1 &&
            index != 0xFFFF) return font;
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        DeleteObject(font);
    }
    return nullptr;
}

// A `size` pixel square, premultiplied 32 bpp bitmap of the glyph in `color`, as themed menus
// draw an item's hbmpItem with its alpha. The glyph is drawn white on black with grayscale
// antialiasing, so any channel is its coverage.
HBITMAP GlyphBitmap(wchar_t glyph, COLORREF color, int size) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    const auto previous = SelectObject(dc, bitmap);
    const auto previousFont = SelectObject(dc, GetStockObject(SYSTEM_FONT));
    HFONT font = SelectIconFont(dc, glyph, size);
    if (font) {
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT box{0, 0, size, size};
        DrawTextW(dc, &glyph, 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        GdiFlush();
        auto* pixel = static_cast<DWORD*>(bits);
        for (int i = 0; i < size * size; ++i) {
            const DWORD coverage = pixel[i] & 0xFF;
            pixel[i] = coverage << 24 | (GetRValue(color) * coverage / 255) << 16 |
                (GetGValue(color) * coverage / 255) << 8 | GetBValue(color) * coverage / 255;
        }
    }
    SelectObject(dc, previousFont);
    if (font) DeleteObject(font);
    SelectObject(dc, previous);
    DeleteDC(dc);
    if (!font) { DeleteObject(bitmap); return nullptr; }
    return bitmap;
}
}

void ApplyMenuTheme(const Appearance& appearance) {
    static const auto setMode = [] {
        const HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
        return theme ? reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(theme, MAKEINTRESOURCEA(135))) : nullptr;
    }();
    static const auto flush = [] {
        const HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
        return theme ? reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(theme, MAKEINTRESOURCEA(136))) : nullptr;
    }();
    if (!setMode || !flush) return;
    // High contrast keeps the system menu; its colors come from the contrast theme.
    setMode(appearance.highContrast ? PreferredAppMode::Default :
        appearance.dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    flush();
}

UINT ShowTrayMenu(HWND owner, POINT anchor, const Appearance& appearance,
                  std::initializer_list<TrayMenuItem> items) {
    ApplyMenuTheme(appearance);
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY))) dpi = GetDpiForSystem();
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    const COLORREF glyphColor = ToColorRef(appearance.foreground);

    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    std::vector<HBITMAP> bitmaps;
    for (const auto& item : items) {
        MENUITEMINFOW info{sizeof(info)};
        if (!item.command) {
            info.fMask = MIIM_FTYPE;
            info.fType = MFT_SEPARATOR;
        } else {
            info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
            info.wID = item.command;
            info.dwTypeData = const_cast<wchar_t*>(item.text);
            info.fState = item.isDefault ? MFS_DEFAULT : MFS_ENABLED;
            // Contrast themes invert the highlighted row, which a fixed-color bitmap cannot follow.
            if (item.glyph && !appearance.highContrast)
                if (HBITMAP bitmap = GlyphBitmap(item.glyph, glyphColor, iconSize)) {
                    info.fMask |= MIIM_BITMAP;
                    info.hbmpItem = bitmap;
                    bitmaps.push_back(bitmap);
                }
        }
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
    }
    // Icons share the check column instead of adding a second gutter.
    MENUINFO style{sizeof(style)};
    style.fMask = MIM_STYLE;
    style.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &style);

    // The menu opens away from the taskbar edge the icon sits on.
    MONITORINFO bounds{sizeof(bounds)};
    GetMonitorInfoW(monitor, &bounds);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    flags |= anchor.y > (bounds.rcMonitor.top + bounds.rcMonitor.bottom) / 2 ? TPM_BOTTOMALIGN : TPM_TOPALIGN;
    flags |= anchor.x > (bounds.rcMonitor.left + bounds.rcMonitor.right) / 2 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    // A notification-area menu closes on an outside click only while its owner is in front.
    SetForegroundWindow(owner);
    const UINT choice = TrackPopupMenu(menu, flags, anchor.x, anchor.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    for (HBITMAP bitmap : bitmaps) DeleteObject(bitmap);
    return choice;
}
}
