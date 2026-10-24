// Generated from design/vi/tokens.json by tools/gen-vi-tokens.ps1. Do not edit.
#pragma once
#include <cstddef>

namespace reel::vi {
struct Color { float r, g, b, a; };
struct Bezier { float x1, y1, x2, y2; };
inline constexpr wchar_t kSchemaVersion[] = L"0.9";
// Geometry (DIP). The reel draws at kReelScale: its effective DPI is round(monitor DPI * kReelScale).
inline constexpr float kReelScale = 0.8f;
// The user's reel size steps multiply kReelScale; kReelSizeDefault indexes the step 1.
inline constexpr float kReelSizeSteps[] = {0.7f, 0.85f, 1.f, 1.15f};
inline constexpr unsigned kReelSizeCount = 4, kReelSizeDefault = 2;
inline constexpr float kCanvas = 384.f, kPlateRadius = 168.f, kOrbitRadius = 112.f, kCenterZoneRadius = 44.f;
inline constexpr float kTile = 40.f, kTileRadius = 10.f, kTileStroke = 1.f;
inline constexpr float kSeat = 48.f, kSeatRadius = 14.f, kSeatStroke = 1.5f, kSeatStrokeHighContrast = 2.5f;
inline constexpr float kGlyphEm = 24.f;              // Segoe Fluent Icons em inside the tile
inline constexpr float kGlyphFallbackStroke = 1.75f, kGlyphFallbackStrokeHighContrast = 2.f;  // vector fallback
inline constexpr float kArtworkKeyline = 28.f;       // glyph-class vendor/system marks: optical box
inline constexpr float kBadgeRadius = 6.f, kBadgeOffset = 17.f, kBadgeRing = 1.5f, kBadgeRingRadius = 7.5f, kBadgeCheckStroke = 1.5f;
inline constexpr float kLabelSize = 14.f, kLabelWidth = 132.f, kLabelHeight = 20.f, kMonogramSize = 16.f, kSizeMonogramSize = 12.f;
inline constexpr float kChevronOffset = 40.f, kChevronEm = 14.f, kChevronHit = 40.f, kBackChevronEm = 14.f;
inline constexpr float kRingStroke = 1.f, kRingGatedStroke = 1.5f, kGateDotRadius = 2.f;
inline constexpr float kPlateStroke = 1.f, kPlateStrokeHighContrast = 2.f;
inline constexpr float kShadowBlur = 16.f, kShadowOffsetY = 8.f;   // Fluent flyout elevation
inline constexpr float kTrashEm = 20.f, kTrashRingStroke = 1.5f, kTrashArmedRadius = 30.f;
inline constexpr float kDisabledOpacity = 0.36f;
// Palettes. Alpha < 1 composites over the plate; selection = Mix(plate, accent, seatTint).
struct Palette { Color plate, foreground, secondary, outline, tile, tileStroke, plateStroke, ringIdle, textOnAccent,
                 critical, criticalFill, textOnCritical;
                 float seatTint, shadowOpacity, frostedTileAlpha; Color accentFallback, accentTextFallback; };
inline constexpr Palette kLight{
    .plate = {0.976f, 0.976f, 0.976f, 1.f},
    .foreground = {0.106f, 0.106f, 0.106f, 1.f},
    .secondary = {0.365f, 0.365f, 0.365f, 1.f},
    .outline = {0.8f, 0.8f, 0.8f, 1.f},
    .tile = {1.f, 1.f, 1.f, 1.f},
    .tileStroke = {0.f, 0.f, 0.f, 0.08f},
    .plateStroke = {0.f, 0.f, 0.f, 0.08f},
    .ringIdle = {0.f, 0.f, 0.f, 0.1f},
    .textOnAccent = {1.f, 1.f, 1.f, 1.f},
    .critical = {0.769f, 0.169f, 0.11f, 1.f},
    .criticalFill = {0.769f, 0.169f, 0.11f, 1.f},
    .textOnCritical = {1.f, 1.f, 1.f, 1.f},
    .seatTint = 0.1f, .shadowOpacity = 0.14f, .frostedTileAlpha = 0.7f,
    .accentFallback = {0.f, 0.373f, 0.722f, 1.f}, .accentTextFallback = {0.f, 0.243f, 0.573f, 1.f},
};
inline constexpr Palette kDark{
    .plate = {0.173f, 0.173f, 0.173f, 1.f},
    .foreground = {1.f, 1.f, 1.f, 1.f},
    .secondary = {0.812f, 0.812f, 0.812f, 1.f},
    .outline = {0.29f, 0.29f, 0.29f, 1.f},
    .tile = {1.f, 1.f, 1.f, 0.1f},
    .tileStroke = {1.f, 1.f, 1.f, 0.08f},
    .plateStroke = {1.f, 1.f, 1.f, 0.1f},
    .ringIdle = {1.f, 1.f, 1.f, 0.1f},
    .textOnAccent = {0.f, 0.f, 0.f, 1.f},
    .critical = {1.f, 0.6f, 0.643f, 1.f},
    .criticalFill = {1.f, 0.6f, 0.643f, 1.f},
    .textOnCritical = {0.f, 0.f, 0.f, 1.f},
    .seatTint = 0.18f, .shadowOpacity = 0.26f, .frostedTileAlpha = 0.1f,
    .accentFallback = {0.376f, 0.804f, 1.f, 1.f}, .accentTextFallback = {0.6f, 0.922f, 1.f, 1.f},
};
// Acrylic (frosted) recipe
inline constexpr Color kAcrylicTintLight{0.988f, 0.988f, 0.988f, 1.f}, kAcrylicTintDark{0.173f, 0.173f, 0.173f, 1.f};
inline constexpr float kAcrylicTintOpacityLight = 0.f, kAcrylicTintOpacityDark = 0.15f;
inline constexpr float kAcrylicLuminosityLight = 0.85f, kAcrylicLuminosityDark = 0.96f, kAcrylicNoiseOpacity = 0.02f;
inline constexpr unsigned kAcrylicNoiseSize = 256;
// Typography
inline constexpr wchar_t kTextFamily[] = L"Segoe UI Variable Text", kTextFallbackFamily[] = L"Segoe UI";
inline constexpr unsigned kTextWeight = 600;
inline constexpr wchar_t kCjkFamily[] = L"Microsoft YaHei UI", kCjkLocale[] = L"zh-CN";
struct UnicodeRange { unsigned first, last; };
inline constexpr UnicodeRange kCjkRanges[] = {{0x2E80, 0x2FDF}, {0x3000, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF}, {0xFF00, 0xFFEF}};
// Command glyphs (Segoe Fluent Icons code points)
inline constexpr wchar_t kIconFamily[] = L"Segoe Fluent Icons", kIconFallbackFamily[] = L"Segoe MDL2 Assets";
inline constexpr wchar_t kGlyphAdd = L'\xE710', kGlyphManage = L'\xF0E2', kGlyphPreferred = L'\xE930', kGlyphSun = L'\xE706', kGlyphMoon = L'\xE708';
inline constexpr wchar_t kGlyphEdit = L'\xE70F', kGlyphRemove = L'\xE74D', kGlyphPrevious = L'\xE76B', kGlyphNext = L'\xE76C';
inline constexpr wchar_t kGlyphChevronLeftSmall = L'\xE76B', kGlyphChevronRightSmall = L'\xE76C', kGlyphBackChevron = L'\xE76B', kGlyphGenericApp = L'\xE737';
inline constexpr wchar_t kGlyphTrayPreview = L'\xE890', kGlyphTraySettings = L'\xE713', kGlyphTrayDiagnostics = L'\xE9D9', kGlyphTrayExit = L'\xE7E8';
// Motion (ms / unitless)
inline constexpr unsigned kOpenFadeMs = 83, kOpenScaleMs = 167;  inline constexpr float kOpenScaleFrom = 0.96f;
inline constexpr unsigned kCloseFadeMs = 83, kCloseScaleMs = 83;  inline constexpr float kCloseScaleTo = 0.98f;
inline constexpr unsigned kCommitHoldMs = 50, kCommitFadeMs = 83, kCommitPressMs = 100, kCommitTotalMs = 133;  inline constexpr float kCommitPressScale = 0.975f;
inline constexpr unsigned kSeatFadeMs = 83, kSeatAppearMs = 167, kTileFillFadeMs = 83;  inline constexpr float kSeatAppearScaleFrom = 0.94f;
inline constexpr float kSeatSpringDamping = 0.85f;  inline constexpr unsigned kSeatSpringPeriodMs = 50;
inline constexpr unsigned kLabelFadeMs = 83, kRingHideMs = 83, kGateArmMs = 167, kGateDotFadeMs = 83;
inline constexpr unsigned kDrillExitMs = 100, kDrillEnterMs = 250, kDrillEnterFadeMs = 83, kDrillEnterDelayMs = 50;
inline constexpr float kDrillInExitScale = 1.04f, kDrillInEnterScale = 0.94f, kDrillOutExitScale = 0.96f, kDrillOutEnterScale = 1.04f;
inline constexpr unsigned kHandoffMs = 200, kHandoffFadeMs = 83, kHandoffLabelFadeMs = 33;  inline constexpr float kHandoffScaleTo = 0.5f;
inline constexpr unsigned kSwapMs = 120, kSwapExitMs = 83;  inline constexpr float kSwapScaleFrom = 0.98f;
inline constexpr unsigned kMonogramFadeMs = 83;
inline constexpr unsigned kDragLiftMs = 120, kDragReflowMs = 167, kDragArmMs = 83, kDragSettleMs = 200, kDragRemoveMs = 167;
inline constexpr float kDragLiftScale = 1.08f, kDragArmScaleFrom = 0.88f, kDragOverTrashScale = 0.7f, kDragOverTrashOpacity = 0.4f, kDragRemoveScaleTo = 0.3f;
inline constexpr unsigned kReducedMotionMs = 1;
inline constexpr Bezier kLinear{0.f, 0.f, 1.f, 1.f};
inline constexpr Bezier kDecelerate{0.f, 0.f, 0.f, 1.f};   // Fluent FastOutSlowIn
inline constexpr Bezier kAccelerate{0.4f, 0.f, 1.f, 1.f};
inline constexpr Bezier kDrillCurve{0.1f, 0.9f, 0.2f, 1.f};    // WinUI DrillIn / Connected
// Per-brand artwork treatment (iconography.brands). Content rect is normalized to the source canvas.
struct BrandTreatment { const wchar_t* key; bool plated; float opticalScale; float contentLeft, contentTop, contentRight, contentBottom; };
inline constexpr BrandTreatment kBrands[] = {
    {L"air", true, 1.f, 0.f, 0.f, 1.f, 1.f},
    {L"antigravity", false, 1.f, 0.12407f, 0.15556f, 0.87222f, 0.84444f},
    {L"chatgpt-dark", false, 0.96f, 0.00391f, 0.00781f, 0.99609f, 0.99219f},
    {L"chatgpt-light", false, 0.96f, 0.00391f, 0.00781f, 0.99609f, 0.99219f},
    {L"claude", true, 1.f, 0.f, 0.f, 1.f, 1.f},
    {L"clion", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"dsh", true, 1.f, 0.03125f, 0.03125f, 0.96875f, 0.96875f},
    {L"gemini", false, 1.04f, 0.04102f, 0.04102f, 0.95898f, 0.95898f},
    {L"goland", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"intellij-idea", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"jetbrains", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"junie", false, 0.92f, 0.f, 0.f, 1.f, 1.f},
    {L"perplexity", true, 1.f, 0.f, 0.f, 1.f, 1.f},
    {L"phpstorm", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"pycharm", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"rider", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"rubymine", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"rustrover", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"toolbox", false, 0.9f, 0.07227f, 0.f, 0.92773f, 1.f},
    {L"vscode", false, 0.9f, 0.f, 0.00195f, 1.f, 0.99805f},
    {L"webstorm", false, 0.86f, 0.f, 0.f, 1.f, 1.f},
    {L"zcode", true, 1.f, 0.f, 0.f, 1.f, 1.f},
};
inline constexpr std::size_t kBrandCount = 22;
}
