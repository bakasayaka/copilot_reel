param(
    [string]$TokensPath,
    [string]$OutputPath,
    # Exit 1 when the committed header differs from the generated text; writes nothing.
    [switch]$Check
)
# Generates src/vi_tokens.h from design/vi/tokens.json. Windows PowerShell 5.1, no Python.
# Output is deterministic: UTF-8 without BOM, LF line endings, written only when it changes.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $TokensPath) { $TokensPath = Join-Path $projectRoot 'design\vi\tokens.json' }
if (-not $OutputPath) { $OutputPath = Join-Path $projectRoot 'src\vi_tokens.h' }
$invariant = [Globalization.CultureInfo]::InvariantCulture
$utf8 = New-Object Text.UTF8Encoding($false)
$tokens = [IO.File]::ReadAllText($TokensPath, $utf8) | ConvertFrom-Json

function Test-Object($value) { $value -is [Management.Automation.PSCustomObject] }
function Get-Token([string]$path) {
    $node = $tokens
    foreach ($part in $path.Split('.')) {
        $property = if (Test-Object $node) { $node.PSObject.Properties[$part] } else { $null }
        if ($null -eq $property) { throw "tokens.json: missing '$path'." }
        $node = $property.Value
    }
    return ,$node
}
function Test-Number($value) { $value -is [int] -or $value -is [long] -or $value -is [decimal] -or $value -is [double] }
function Get-Number([string]$path) {
    $value = Get-Token $path
    if (-not (Test-Number $value)) { throw "tokens.json: '$path' must be a number." }
    [double]$value
}
function Format-Float([double]$value) {
    $text = $value.ToString('R', $invariant)
    if ($text -notmatch '[.E]') { $text += '.' }
    $text + 'f'
}
function Float([string]$path) { Format-Float (Get-Number $path) }
function UInt([string]$path) {
    $value = Get-Number $path
    if ($value -lt 0 -or $value -ne [Math]::Floor($value)) { throw "tokens.json: '$path' must be a non-negative integer." }
    $value.ToString('0', $invariant)
}
function Unit([string]$path) {
    $value = Get-Number $path
    if ($value -lt 0 -or $value -gt 1) { throw "tokens.json: '$path' must be within 0..1." }
    Format-Float $value
}
function Color([string]$path) {
    $value = Get-Token $path
    $alpha = 1.0
    if ($value -is [string]) { $hex = $value } else { $hex = Get-Token "$path.hex"; $alpha = Get-Number "$path.alpha" }
    if ($hex -isnot [string] -or $hex -notmatch '^#[0-9A-Fa-f]{6}$') { throw "tokens.json: '$path' must be '#RRGGBB' or {hex, alpha}." }
    if ($alpha -lt 0 -or $alpha -gt 1) { throw "tokens.json: '$path' alpha must be within 0..1." }
    $channels = @(1, 3, 5 | ForEach-Object { [Convert]::ToInt32($hex.Substring($_, 2), 16) / 255.0 }) + $alpha
    '{' + ((@($channels) | ForEach-Object { Format-Float ([Math]::Round($_, 3)) }) -join ', ') + '}'
}
function Curve([string]$name) {
    $points = Get-Token "motion.curves.$name"
    $points = @($points)
    if ($points.Count -ne 4 -or @($points | Where-Object { -not (Test-Number $_) }).Count) { throw "tokens.json: 'motion.curves.$name' must be [x1, y1, x2, y2]." }
    if ([double]$points[0] -lt 0 -or [double]$points[0] -gt 1 -or [double]$points[2] -lt 0 -or [double]$points[2] -gt 1) { throw "tokens.json: 'motion.curves.$name' x values must be within 0..1." }
    '{' + ((@($points) | ForEach-Object { Format-Float ([double]$_) }) -join ', ') + '}'
}
function Text([string]$path) {
    $value = Get-Token $path
    if ($value -isnot [string] -or $value -notmatch '^[A-Za-z0-9 ._:!+-]+\z') { throw "tokens.json: '$path' must be a plain ASCII name." }
    'L"' + $value + '"'
}
function Glyph([string]$name) {
    $value = Get-Token "iconography.commandGlyphs.$name"
    if ($value -isnot [string] -or $value -notmatch '^[0-9A-Fa-f]{4}$') { throw "tokens.json: 'iconography.commandGlyphs.$name' must be a 4-digit hex code point." }
    "L'\x" + $value.ToUpperInvariant() + "'"
}
function Palette([string]$theme) {
    $p = "palettes.$theme"
    @("inline constexpr Palette k$($theme.Substring(0, 1).ToUpperInvariant() + $theme.Substring(1)){") +
    @('plate', 'foreground', 'secondary', 'outline', 'tile', 'tileStroke', 'plateStroke', 'ringIdle', 'textOnAccent',
      'critical', 'criticalFill', 'textOnCritical' |
        ForEach-Object { "    .$_ = $(Color "$p.$_")," }) +
    @("    .seatTint = $(Unit "$p.seatTint"), .shadowOpacity = $(Unit "$p.shadowOpacity"), .frostedTileAlpha = $(Unit "$p.frostedTileAlpha"),",
      "    .accentFallback = $(Color "$p.accent.fallback"), .accentTextFallback = $(Color "$p.accentText.fallback"),",
      '};')
}

$commit = 'motion.events.commit'
if ((Get-Number "$commit.totalMs") -ne (Get-Number "$commit.holdMs") + (Get-Number "$commit.fadeMs")) {
    throw "tokens.json: '$commit.totalMs' must equal holdMs + fadeMs."
}
$ranges = Get-Token 'typography.cjk.ranges'
$ranges = @(@($ranges) | ForEach-Object {
    if ($_ -isnot [string] -or $_ -notmatch '^([0-9A-Fa-f]{4,6})-([0-9A-Fa-f]{4,6})$') { throw "tokens.json: 'typography.cjk.ranges' entries must be 'XXXX-YYYY'." }
    if ([Convert]::ToInt32($Matches[1], 16) -gt [Convert]::ToInt32($Matches[2], 16)) { throw "tokens.json: CJK range '$_' is reversed." }
    '{0x' + $Matches[1].ToUpperInvariant() + ', 0x' + $Matches[2].ToUpperInvariant() + '}'
})
$brandLines = New-Object Collections.Generic.List[string]
$brandKeys = @{}
$brands = Get-Token 'iconography.brands'
foreach ($brand in @($brands)) {
    $index = $brandLines.Count
    $where = "iconography.brands[$index]"
    if (-not (Test-Object $brand)) { throw "tokens.json: '$where' must be an object." }
    foreach ($name in 'key', 'class', 'opticalScale', 'content') {
        if ($null -eq $brand.PSObject.Properties[$name]) { throw "tokens.json: '$where' is missing '$name'." }
    }
    $key = $brand.key
    if ($key -isnot [string] -or $key -notmatch '^[A-Za-z0-9._:!+-]+\z') { throw "tokens.json: '$where.key' must use A-Z a-z 0-9 . _ : ! + -." }
    if ($brandKeys.ContainsKey($key)) { throw "tokens.json: brand key '$key' is duplicated." }
    $brandKeys[$key] = $true
    if ($brand.class -cne 'plated' -and $brand.class -cne 'glyph') { throw "tokens.json: '$where.class' must be 'plated' or 'glyph'." }
    if (-not (Test-Number $brand.opticalScale) -or [double]$brand.opticalScale -le 0 -or [double]$brand.opticalScale -gt 2) { throw "tokens.json: '$where.opticalScale' must be within (0, 2]." }
    $content = @($brand.content)
    if ($content.Count -ne 4 -or @($content | Where-Object { -not (Test-Number $_) -or [double]$_ -lt 0 -or [double]$_ -gt 1 }).Count) { throw "tokens.json: '$where.content' must be four numbers within 0..1." }
    if ([double]$content[0] -ge [double]$content[2] -or [double]$content[1] -ge [double]$content[3]) { throw "tokens.json: '$where.content' must have left < right and top < bottom." }
    $plated = if ($brand.class -ceq 'plated') { 'true' } else { 'false' }
    $brandLines.Add("    {L`"$key`", $plated, $(Format-Float ([double]$brand.opticalScale)), " + ((@($content) | ForEach-Object { Format-Float ([double]$_) }) -join ', ') + '},')
}
$sizeSteps = Get-Token 'geometry.sizeSteps'
$sizeSteps = @($sizeSteps)
if ($sizeSteps.Count -lt 1 -or @($sizeSteps | Where-Object { -not (Test-Number $_) -or [double]$_ -le 0 -or [double]$_ -gt 2 }).Count) { throw "tokens.json: 'geometry.sizeSteps' must be numbers within (0, 2]." }
for ($i = 1; $i -lt $sizeSteps.Count; ++$i) { if ([double]$sizeSteps[$i] -le [double]$sizeSteps[$i - 1]) { throw "tokens.json: 'geometry.sizeSteps' must ascend." } }
$sizeDefault = [int](UInt 'geometry.sizeDefault')
if ($sizeDefault -ge $sizeSteps.Count -or [double]$sizeSteps[$sizeDefault] -ne 1) { throw "tokens.json: 'geometry.sizeDefault' must index the step 1." }
$brandCount = $brandLines.Count
if ($brandCount -eq 0) { $brandLines.Add('    {nullptr, false, 1.f, 0.f, 0.f, 1.f, 1.f},  // sentinel: no brands yet; kBrandCount is 0') }

$g = 'geometry'
$m = 'motion.events'
$f = 'systemAppearance.frosted'
$label = 'typography.applicationLabel'
$lines = @(
    '// Generated from design/vi/tokens.json by tools/gen-vi-tokens.ps1. Do not edit.'
    '#pragma once'
    '#include <cstddef>'
    ''
    'namespace reel::vi {'
    'struct Color { float r, g, b, a; };'
    'struct Bezier { float x1, y1, x2, y2; };'
    "inline constexpr wchar_t kSchemaVersion[] = $(Text 'schemaVersion');"
    '// Geometry (DIP). The reel draws at kReelScale: its effective DPI is round(monitor DPI * kReelScale).'
    "inline constexpr float kReelScale = $(Unit "$g.scale");"
    '// The user''s reel size steps multiply kReelScale; kReelSizeDefault indexes the step 1.'
    "inline constexpr float kReelSizeSteps[] = {$((@($sizeSteps) | ForEach-Object { Format-Float ([double]$_) }) -join ', ')};"
    "inline constexpr unsigned kReelSizeCount = $($sizeSteps.Count), kReelSizeDefault = $sizeDefault;"
    "inline constexpr float kCanvas = $(Float "$g.canvas"), kPlateRadius = $(Float "$g.plate.radius"), kOrbitRadius = $(Float "$g.orbit.radius"), kCenterZoneRadius = $(Float "$g.centerZone.radius");"
    "inline constexpr float kTile = $(Float "$g.tile.size"), kTileRadius = $(Float "$g.tile.radius"), kTileStroke = $(Float "$g.tile.stroke");"
    "inline constexpr float kSeat = $(Float "$g.seat.size"), kSeatRadius = $(Float "$g.seat.radius"), kSeatStroke = $(Float "$g.seat.stroke"), kSeatStrokeHighContrast = $(Float "$g.seat.strokeHighContrast");"
    "inline constexpr float kGlyphEm = $(Float "$g.glyph.em");              // Segoe Fluent Icons em inside the tile"
    "inline constexpr float kGlyphFallbackStroke = $(Float "$g.glyph.fallbackStroke"), kGlyphFallbackStrokeHighContrast = $(Float "$g.glyph.fallbackStrokeHighContrast");  // vector fallback"
    "inline constexpr float kArtworkKeyline = $(Float "$g.artwork.keyline");       // glyph-class vendor/system marks: optical box"
    "inline constexpr float kBadgeRadius = $(Float "$g.badge.radius"), kBadgeOffset = $(Float "$g.badge.offset"), kBadgeRing = $(Float "$g.badge.ring"), kBadgeRingRadius = $(Float "$g.badge.ringRadius"), kBadgeCheckStroke = $(Float "$g.badge.checkStroke");"
    "inline constexpr float kLabelSize = $(Float "$label.size"), kLabelWidth = $(Float "$label.width"), kLabelHeight = $(Float "$label.height"), kMonogramSize = $(Float 'typography.iconMonogram.size'), kSizeMonogramSize = $(Float 'typography.iconMonogram.sizeStep.size');"
    "inline constexpr float kChevronOffset = $(Float "$g.chevrons.offset"), kChevronEm = $(Float "$g.chevrons.em"), kChevronHit = $(Float "$g.chevrons.hit"), kBackChevronEm = $(Float "$g.chevrons.backEm");"
    "inline constexpr float kRingStroke = $(Float "$g.centerZone.ringStroke"), kRingGatedStroke = $(Float "$g.centerZone.ringGatedStroke"), kGateDotRadius = $(Float "$g.centerZone.gateDotRadius");"
    "inline constexpr float kPlateStroke = $(Float "$g.plate.stroke"), kPlateStrokeHighContrast = $(Float "$g.plate.strokeHighContrast");"
    "inline constexpr float kShadowBlur = $(Float 'elevation.shadowBlur'), kShadowOffsetY = $(Float 'elevation.shadowOffsetY');   // Fluent flyout elevation"
    "inline constexpr float kTrashEm = $(Float "$g.trash.em"), kTrashRingStroke = $(Float "$g.trash.ringStroke"), kTrashArmedRadius = $(Float "$g.trash.armedRadius");"
    "inline constexpr float kDisabledOpacity = $(Unit "$g.disabledOpacity");"
    '// Palettes. Alpha < 1 composites over the plate; selection = Mix(plate, accent, seatTint).'
    'struct Palette { Color plate, foreground, secondary, outline, tile, tileStroke, plateStroke, ringIdle, textOnAccent,'
    '                 critical, criticalFill, textOnCritical;'
    '                 float seatTint, shadowOpacity, frostedTileAlpha; Color accentFallback, accentTextFallback; };'
) + (Palette 'light') + (Palette 'dark') + @(
    '// Acrylic (frosted) recipe'
    "inline constexpr Color kAcrylicTintLight$(Color "$f.light.tint"), kAcrylicTintDark$(Color "$f.dark.tint");"
    "inline constexpr float kAcrylicTintOpacityLight = $(Unit "$f.light.tintOpacity"), kAcrylicTintOpacityDark = $(Unit "$f.dark.tintOpacity");"
    "inline constexpr float kAcrylicLuminosityLight = $(Unit "$f.light.luminosityOpacity"), kAcrylicLuminosityDark = $(Unit "$f.dark.luminosityOpacity"), kAcrylicNoiseOpacity = $(Unit "$f.noise.opacity");"
    "inline constexpr unsigned kAcrylicNoiseSize = $(UInt "$f.noise.size");"
    '// Typography'
    "inline constexpr wchar_t kTextFamily[] = $(Text 'typography.preferred'), kTextFallbackFamily[] = $(Text 'typography.fallback');"
    "inline constexpr unsigned kTextWeight = $(UInt 'typography.weight');"
    "inline constexpr wchar_t kCjkFamily[] = $(Text 'typography.cjk.family'), kCjkLocale[] = $(Text 'typography.cjk.locale');"
    'struct UnicodeRange { unsigned first, last; };'
    "inline constexpr UnicodeRange kCjkRanges[] = {$($ranges -join ', ')};"
    '// Command glyphs (Segoe Fluent Icons code points)'
    "inline constexpr wchar_t kIconFamily[] = $(Text 'iconography.commandFont'), kIconFallbackFamily[] = $(Text 'iconography.commandFontFallback');"
    "inline constexpr wchar_t kGlyphAdd = $(Glyph Add), kGlyphManage = $(Glyph Manage), kGlyphPreferred = $(Glyph Preferred), kGlyphSun = $(Glyph Sun), kGlyphMoon = $(Glyph Moon);"
    "inline constexpr wchar_t kGlyphEdit = $(Glyph Edit), kGlyphRemove = $(Glyph Remove), kGlyphPrevious = $(Glyph Previous), kGlyphNext = $(Glyph Next);"
    "inline constexpr wchar_t kGlyphChevronLeftSmall = $(Glyph ChevronLeftSmall), kGlyphChevronRightSmall = $(Glyph ChevronRightSmall), kGlyphBackChevron = $(Glyph BackChevron), kGlyphGenericApp = $(Glyph GenericApp);"
    "inline constexpr wchar_t kGlyphTrayPreview = $(Glyph TrayPreview), kGlyphTraySettings = $(Glyph TraySettings), kGlyphTrayDiagnostics = $(Glyph TrayDiagnostics), kGlyphTrayExit = $(Glyph TrayExit);"
    '// Motion (ms / unitless)'
    "inline constexpr unsigned kOpenFadeMs = $(UInt "$m.open.fadeMs"), kOpenScaleMs = $(UInt "$m.open.scaleMs");  inline constexpr float kOpenScaleFrom = $(Float "$m.open.scaleFrom");"
    "inline constexpr unsigned kCloseFadeMs = $(UInt "$m.close.fadeMs"), kCloseScaleMs = $(UInt "$m.close.scaleMs");  inline constexpr float kCloseScaleTo = $(Float "$m.close.scaleTo");"
    "inline constexpr unsigned kCommitHoldMs = $(UInt "$commit.holdMs"), kCommitFadeMs = $(UInt "$commit.fadeMs"), kCommitPressMs = $(UInt "$commit.pressMs"), kCommitTotalMs = $(UInt "$commit.totalMs");  inline constexpr float kCommitPressScale = $(Float "$commit.pressScale");"
    "inline constexpr unsigned kSeatFadeMs = $(UInt "$m.selection.seatFadeMs"), kSeatAppearMs = $(UInt "$m.selection.seatAppearMs"), kTileFillFadeMs = $(UInt "$m.selection.tileFillFadeMs");  inline constexpr float kSeatAppearScaleFrom = $(Float "$m.selection.seatAppearScaleFrom");"
    "inline constexpr float kSeatSpringDamping = $(Float "$m.selection.travel.dampingRatio");  inline constexpr unsigned kSeatSpringPeriodMs = $(UInt "$m.selection.travel.periodMs");"
    "inline constexpr unsigned kLabelFadeMs = $(UInt "$m.centerLabel.crossfadeMs"), kRingHideMs = $(UInt "$m.centerLabel.ringHideMs"), kGateArmMs = $(UInt "$m.gate.armMs"), kGateDotFadeMs = $(UInt "$m.gate.dotFadeMs");"
    "inline constexpr unsigned kDrillExitMs = $(UInt "$m.drill.exitMs"), kDrillEnterMs = $(UInt "$m.drill.enterMs"), kDrillEnterFadeMs = $(UInt "$m.drill.enterFadeMs"), kDrillEnterDelayMs = $(UInt "$m.drill.enterDelayMs");"
    "inline constexpr float kDrillInExitScale = $(Float "$m.drillIn.exitScale"), kDrillInEnterScale = $(Float "$m.drillIn.enterScaleFrom"), kDrillOutExitScale = $(Float "$m.drillOut.exitScale"), kDrillOutEnterScale = $(Float "$m.drillOut.enterScaleFrom");"
    "inline constexpr unsigned kHandoffMs = $(UInt "$m.handoff.travelMs"), kHandoffFadeMs = $(UInt "$m.handoff.fadeMs"), kHandoffLabelFadeMs = $(UInt "$m.handoff.labelFadeMs");  inline constexpr float kHandoffScaleTo = $(Float "$m.handoff.scaleTo");"
    "inline constexpr unsigned kSwapMs = $(UInt "$m.swap.enterMs"), kSwapExitMs = $(UInt "$m.swap.exitMs");  inline constexpr float kSwapScaleFrom = $(Float "$m.swap.enterScaleFrom");"
    "inline constexpr unsigned kMonogramFadeMs = $(UInt "$m.language.monogramCrossfadeMs");"
    "inline constexpr unsigned kDragLiftMs = $(UInt "$m.drag.liftMs"), kDragReflowMs = $(UInt "$m.drag.reflowMs"), kDragArmMs = $(UInt "$m.drag.armMs"), kDragSettleMs = $(UInt "$m.drag.settleMs"), kDragRemoveMs = $(UInt "$m.drag.removeMs");"
    "inline constexpr float kDragLiftScale = $(Float "$m.drag.liftScale"), kDragArmScaleFrom = $(Float "$m.drag.armScaleFrom"), kDragOverTrashScale = $(Float "$m.drag.overTrashScale"), kDragOverTrashOpacity = $(Unit "$m.drag.overTrashOpacity"), kDragRemoveScaleTo = $(Float "$m.drag.removeScaleTo");"
    "inline constexpr unsigned kReducedMotionMs = $(UInt "$m.reducedMotion.durationMs");"
    "inline constexpr Bezier kLinear$(Curve linear);"
    "inline constexpr Bezier kDecelerate$(Curve decelerate);   // Fluent FastOutSlowIn"
    "inline constexpr Bezier kAccelerate$(Curve accelerate);"
    "inline constexpr Bezier kDrillCurve$(Curve drill);    // WinUI DrillIn / Connected"
    '// Per-brand artwork treatment (iconography.brands). Content rect is normalized to the source canvas.'
    'struct BrandTreatment { const wchar_t* key; bool plated; float opticalScale; float contentLeft, contentTop, contentRight, contentBottom; };'
    'inline constexpr BrandTreatment kBrands[] = {'
) + $brandLines + @(
    '};'
    "inline constexpr std::size_t kBrandCount = $brandCount;"
    '}'
)
$text = ($lines -join "`n") + "`n"
$bytes = $utf8.GetBytes($text)
$current = if (Test-Path -LiteralPath $OutputPath) { [IO.File]::ReadAllBytes($OutputPath) } else { $null }
$same = $null -ne $current -and [Convert]::ToBase64String($current) -ceq [Convert]::ToBase64String($bytes)
if ($Check) {
    if (-not $same) {
        [Console]::Error.WriteLine("$OutputPath is out of date with $TokensPath. Run tools/gen-vi-tokens.ps1.")
        exit 1
    }
    Write-Output "Up to date: $OutputPath"
    exit 0
}
if (-not $same) { [IO.File]::WriteAllBytes($OutputPath, $bytes) }
Write-Output "Generated: $OutputPath"
