#pragma once
#include "app_types.h"
#include <initializer_list>

// The notification-area menu: a native popup menu drawn in the reel's light or dark
// theme, with Segoe Fluent Icons beside its commands.
namespace reel {
struct TrayMenuItem {
    UINT command = 0;          // 0 is a separator
    const wchar_t* text = nullptr;
    wchar_t glyph = 0;         // Segoe Fluent Icons code point; 0 draws no icon
    bool isDefault = false;    // bold; the command a click on the icon runs
};

// Popup menus follow the process' preferred app mode: this sets it to the appearance's theme,
// so the tray menu and the editor's context menus match the reel. High contrast keeps the
// system menus.
void ApplyMenuTheme(const Appearance& appearance);

// Shows the menu at `anchor` (screen pixels, the icon's anchor point) and returns the chosen
// command, or 0. `owner` must be the window that receives the icon's messages. Applies the
// menu theme first; high contrast shows no icons.
UINT ShowTrayMenu(HWND owner, POINT anchor, const Appearance& appearance,
                  std::initializer_list<TrayMenuItem> items);
}
