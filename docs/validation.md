# Validation status

Snapshot: 2026-10-23, Windows 11 25H2 build `26200.9711`, x64. Installed application: **0.3.6.1**. This records demonstrated behavior and its limits, not full release acceptance.

## Installed-app picker and editor layout (fork contribution, 2026-10-24)

Recorded on a contributor's ARM64 Windows 11 PC (NVIDIA RTX Spark N1X, 200% scaling), not the x64 machine above. The package is `0.3.6.1` ARM64 built from branch `feature/installed-app-picker`, signed with that machine's own development certificate.

| Area | Evidence and scope |
| --- | --- |
| ARM64 on ARM | First run of the ARM64 package on ARM hardware: installed, registered (`ProviderDeclarationValid`, `SelectedByUser` true), started in the background and bound to the Copilot key. All eight unit-test executables ran natively and passed; the installed executable equals the build. |
| Picker | `installed_apps_tests` classified entries recorded from this machine's Start and listed 114 of its 129 All apps entries; every listed app passed `IsVerifiedLocalApplication`. The user picked Claude (a package) in the real editor and saw its AUMID filled in. |
| Editor layout | A scratch harness opened the focused editor off-screen with an isolated `COPILOT_REEL_DATA_DIR` and captured the Edit, Advanced and Choose app pages, including search, an empty result and a pick, in English and Chinese with `PrintWindow`. Fixed from those captures: text fields drew square borders with their text at the top; they are now rounded painted frames with a centered borderless EDIT and an accent ring while focused. Every page uses one grid (72 DIP rows from 72, a footer at 304), with square 32 DIP header buttons, equal 8 DIP gaps beside the target, Advanced in the footer, a 32 DIP combo box, whole list rows, dark list scrollbars in dark mode, an explained empty or loading list, a disabled Choose that no longer looks primary, and Up/Down from the search box. |
| Tray menu | The notification-area menu was a plain `TrackPopupMenu` menu that stayed light on this dark desktop: the process never set a preferred app mode. `ShowTrayMenu` (`src/tray_menu.cpp`) now sets uxtheme's `SetPreferredAppMode` (ordinal 135, force dark or light from the reel's appearance; default in high contrast) and `FlushMenuThemes` (136) before each menu and on every appearance change, so the editor's context menus follow too. Commands carry Segoe Fluent glyphs at the small-icon size as premultiplied bitmaps sharing the check column, Preview is bold as the click default, and the menu opens from the icon's anchor (`WM_CONTEXTMENU` wParam) away from the taskbar edge. A scratch harness linking `tray_menu.cpp` showed the menu above the tray, highlighted a row with `MN_SELECTITEM` and captured it from the screen in dark and light, English and Chinese, at 200%. |
| Reel size | The hub gained Reel size: 70%, 85%, 100% (the previous 0.8 scale) and 115%, stored as `ReelSize` in `preferences.ini`. The step multiplies the reel's effective DPI, so placement, the edge clamp and the work-area shrink are the opening's own (`PlaceReel`, shared by `Start` and `NextReelSize`). A C++ seam client started an unpackaged test instance with an isolated data directory and clicked the tile through every step with test packets only, with the hub opened at the work-area center and 20 px inside its bottom-right corner (work area 2880 x 1704 at 200%): windows of 616, 708, 432, 524 and 616 px, every one inside the work area (in the corner flush with its right and bottom edges), the hub names unchanged, and Windows Graphics Capture stills showing the monogram, the hover name and the kept selection. Behavior tests round-trip both preferences (saving either keeps the other; an unknown percentage selects 100%); core tests check the five-item hub. `tests/run-reel-seam.py` was updated for the five-item hub but not run: this machine has no Python. |
| Picker scrolling | Touchpad scrolling in the Choose app list felt rough: a LISTBOX scrolls by whole rows only. A probe feeding a LISTBOX 15-unit wheel deltas (a precision touchpad's size) showed it accumulating them and then jumping 68 px (one 34 DIP row at 200%) every 40-45 units, with nothing in between; painting a row costs about 1.2 ms, so drawing was not the cause. The LISTBOX is now as tall as its rows inside a `CopilotReel.ListViewport` child that moves it by pixels and owns the themed scroll bar; each wheel delta moves the list by its share of the system's lines per notch (a 15-unit delta moves 25 px, a mouse notch still three rows), PageUp/PageDown page by the viewport, and a selection changed by keys or clicks scrolls into view. Icons that arrive while scrolling repaint only their row. An off-screen harness with an isolated `COPILOT_REEL_DATA_DIR` drove 132 Start apps with messages only: offsets 25, 51, 76 … per delta, reversal, clamping at both ends, the scroll bar, Down, PageDown, End, Home and search all behaved, at 1.9 ms per delta. |

Not verified: light theme and high contrast (the machine stays dark; the tray menu's light theme was forced in the harness only), scaling other than 200%, keyboard-only use of the picker by hand, the restyled tray menu from a real right-click, and the picker's scrolling with the physical touchpad. The LISTBOX is one window as tall as all its rows, so a list of more than about 480 apps at 200% would exceed 32767 px.

## Verified in v0.3.6.1

The user reported that after installing 0.3.6 a Copilot hold showed nothing while a tap still opened ChatGPT. `run.log` recorded every hold (`Hold started`, then `Gesture canceled before selection`), and a capture of the reel's own window showed it fully drawn. It was not topmost: the reel calls `SetWindowPos(HWND_TOPMOST)` on every opening, and Windows ignores that call, while still returning success, for a process without foreground rights. A background start (the startup task, or `tools/open-installed.ps1 -View Background` after an install) has none, so the reel opened beneath the maximized Claude window. Calling `SetWindowPos` on it from outside also left it non-topmost, while the same call made a fresh test instance or Notepad topmost. Opening the preview once from Start granted the rights. This had been latent since 0.3.5 added the startup task; an earlier resident reel had presumably been raised once by a preview and kept `WS_EX_TOPMOST`.

| Area | Evidence and scope |
| --- | --- |
| Fix | The window is created with `WS_EX_TOPMOST`. `Start` logs a warning if it ever opens without it, and diagnostics report `topmost=`. |
| Interaction | `tests/run-reel-seam.py` passed all 12 groups (`artifacts/qa/seam-20261023-215448-1fdedd/`); preflight now asserts that the test window is topmost before it first opens. |
| Installation | 0.3.6.1.0 replaced 0.3.6.0 after a backup (`artifacts/backups/20261023-215519`), and was started in the background exactly as before. A HoldStart command then opened it topmost, with no ordinary window above it; the installed executable equals the tested build. |
| ARM64 package | Cross-compiled (machine `0xAA64`, not run), signed with the development certificate, `signtool verify /pa` passed; executable SHA-256 `11D6C382…D338`. |

Not yet verified: a physical Copilot hold after a reboot, when the startup task starts the reel.

## Verified in v0.3.6

0.3.6 follows four requests from the user on 2026-10-23: the reel was too large on this 1440 x 900 DIP (200%) display; apps should be reordered by dragging their tiles on Manage apps, with a red trash in the center that takes an app out of the reel while keeping its settings for Add; the reorder chevrons on an app's actions page go; and the hub's Preferred agent entry goes, leaving Set as preferred on an app's actions page.

| Area | Evidence and scope |
| --- | --- |
| Native build | MSVC `/W4` without warnings. Unit tests: core (`MoveEntry`/`MovedIndex` for every from/to pair at 1-8 apps, `DropSlot` on every slot and both halves of Add, `Park` order and the 16-app cap, `AddCatalog` with parked apps first and a parked profile of an app still in the reel, `RouteKey` A/D/Delete on Manage, the four-item hub, `MenuPage::Entry` still 5), settings 64 (parked.ini 0-16 entries, padded and signed counts, format separation, an atomic round trip in a private temporary directory), behavior 65, iconography 182, local launch 233, display names 51, test-input seam (Press 4, Release 5). |
| Interaction | `tests/run-reel-seam.py` passed all 12 groups (`artifacts/qa/seam-20261023-213557-d43c52/`). Group `c` was rewritten for 0.3.6: no reorder on an app's actions page; a press and release in place is a click; a drag through the seam's new Press and Release actions shows the idle trash, saves the order on release with a reflow and keeps the preferred agent; a drag back to its own slot or canceled with Escape saves nothing and its release is not a click; a drop on the armed trash parks the app in `parked.ini`; Add lists it first and one tap restores its target and arguments without the editor; A/D and Delete on Manage; Remove from reel parks too; with one app the trash is disabled. `preferred` and `cold` set the preferred agent from an app's actions page. |
| Rendering | `tools/capture-seam.py` published `output/runtime/v0.3.6/` (seven variants, eleven stills each, including `manage-drag` and `manage-trash`) with `--no-reference`, then a second run compared every still with it: 0 pixels beyond tolerance in every variant (`artifacts/qa/capture-seam-20261023-213824-f33ca8/`). The 0.8 scale makes the window 616 px at 192 DPI (768 px in 0.3.5) and 308 px at 96 DPI. Tile and seat edges are snapped to device pixels at the new effective DPIs. |
| Review | Five lenses (input state machine, renderer, persistence, scale, tests) with adversarial verification. Fixed: keys during a pending press reached a stale drag index; a Copilot hold during a pending press could leave the hold stuck; an appearance or device rebuild during a drag could park an app with no trash drawn; the seam's Release missed the capture of a pending press; Reflow jumped tiles to their targets instead of starting where they were drawn, and could restart a trashed duplicate; the drill-in handoff started from a stale offset; an unreadable `parked.ini` would have been overwritten (it is now backed up first); a trailing `/` in `COPILOT_REEL_DATA_DIR` misplaced `parked.ini`; parked.ini's byte limit could block the trash; artwork warm-up used the monitor DPI instead of the reel's; the tiny-work-area fallback could round past the work area; the documented seam barrier. |
| Clean installation | 0.3.6.0 replaced 0.3.5.1.0 after a backup (`artifacts/backups/20261023-214113`); three configuration files were restored. Installed executable SHA-256 `6F327875…D061` equals the tested build; the Copilot key still selects `CopilotReel.Desktop_nks6vs6sjb1qw!App`. |

| ARM64 package | Cross-compiled (machine `0xAA64`, not run), signed with the development certificate, `signtool verify /pa` passed; executable SHA-256 `A4840044…F063C`. |

Not yet verified: a real mouse drag on this machine (capture, `WM_CAPTURECHANGED`, the system drag threshold); the drag with a pen or touch; whether 0.8 is the size the user wants.

## Verified in v0.3.5.1

0.3.5.1 fixes the Add page artwork, changes edge gating at the user's request, and resolves most known limits of 0.3.5. The user found the artwork defect: on the Add page the not-installed Antigravity, JetBrains IDE and Junie entries all showed Antigravity's mark, because the renderer cached tile artwork by launch target and all three have an empty one. No 0.3.5 check looked at Add page pixels.

| Area | Evidence and scope |
| --- | --- |
| Native build | MSVC `/W4` without warnings. Unit tests: core (gate at the plate radius, `ParseUri` with `&inputTimestamp=`), settings 48, behavior 65, iconography 182 (`ArtworkIdentity`: empty-target entries keep distinct marks and pixels; network targets are never looked up), local launch 233 (`IsNetworkPath`, `ShortcutArguments`), display names 51, test-input seam. |
| Interaction | `tests/run-reel-seam.py` passed all 12 groups (`artifacts/qa/seam-20261022-162937-f99a2a/`), including holds 30 DIP inside the right and top edges: the pointer stays on the plate, an unmoved release cancels and a 10 DIP move selects and launches once. |
| Rendering | `tools/capture-seam.py`, all seven variants (`artifacts/qa/capture-seam-20261022-162957-7caae7/`): every still that exists in 0.3.4 is within 2 levels per channel, compared in every channel after a 1 s settle. The Add page tile check passed on every asserted variant. Frosted variants record one cross-page mismatch for `Custom app`, which sits at different slots over the acrylic noise, so their tile check is recorded, not asserted. A build with the 0.3.5 target-keyed cache put back fails the check with exactly Antigravity, JetBrains IDE and Junie (`artifacts/qa/capture-seam-20261022-162819-1143e3/`, exit 1). |
| Start entry research | Reverse-engineered `SettingsHandlers_nt.dll` 10.0.26100.9711: the Settings Copilot-key picker skips a provider whose declaring application has `AppListEntry="none"`, so a hidden provider with a separate Start entry is not possible. `twinui.pcshell.dll` finds the fast-path window by the owning process's exact AUMID and activates the provider's AUMID without arguments only when its cached SingleTap URI is empty; it appends `&inputTimestamp=` behind a feature flag, which 0.3.5 rejected. |
| Review | Two review lenses with adversarial verification. Fixed: the taskbar counted as a deliberate start (a key tap after Win+T would have opened the preview), chained shortcut arguments and entry working directories ignored by the raise, Shell icon lookups on network shares, an Add tile check limited to one page, and documentation that overstated the raise and the tile check. |
| Clean installation | 0.3.5.1.0 replaced 0.3.5.0 after a backup (`artifacts/backups/20261022-163318`); three configuration files were restored. Installed executable SHA-256 `667676A2…7E80` equals the tested build, all 42 packaged asset files match, the provider declaration is valid, the Copilot key selection is unchanged, and the startup task state is `2` (enabled) before and after. |
| ARM64 package | Cross-compiled (machine `0xAA64`, not run), signed with the development certificate, `signtool verify /pa` passed; executable SHA-256 `19582E8E…B5A8`. |

Resolved from the 0.3.5 limits: launcher stubs and packaged agents are brought forward; network executables are neither checked nor looked up on the UI thread; a reinstall turns a turned-off startup task off again (`--disable-startup-task`); a cold start from Start or search opens the preview.

Not yet verified: physical Copilot-key timing; whether Start or search is still the foreground window when a cold start from Start begins (`run.log` records `foreground=` for every argument-less start); the raise of a running packaged agent or launcher stub with the real key; the startup-task restore on a machine where the task was off.

## Verified in v0.3.5

0.3.5 gives every input one meaning in every menu, classifies argument-less starts, adds a startup task, ignores the Shell's minimize of the provider window, brings a running desktop agent forward, discovers installed agents for one-tap adding, and moves an app from its actions page. Two field defects from 0.3.4.1 led to it: after a cold start the Shell's argument-less fallback opened a preview that stayed 33 s, and after a tap the Shell minimized the reel window, so the next hold opened at the old position on the other monitor.

| Area | Evidence and scope |
| --- | --- |
| Root causes | `twinui.pcshell.dll` 10.0.26100.9278 activates the provider's AUMID with no arguments when no fast-path window exists, and after a fast-path tap sends `SC_MINIMIZE` to a provider window that has the foreground (static analysis; the scratch reproduction with the reel's window styles matched the logged rects exactly). |
| Native build | MSVC `/W4` without warnings. Unit tests: core (now including `ReelInput`, `RouteKey`, `AdjustFor`, `MoveEntry`, `AddCatalog` and `CanConfirm`), settings 48, behavior 65, iconography 137, local launch 218 (`InstalledIdentity`, the catalog rules and candidate filter), display names 51, test-input seam (gate and packet decoding). |
| Interaction | `tests/run-reel-seam.py` passed all 12 groups without moving the pointer, sending input or changing the foreground, while the installed reel stayed resident (`artifacts/qa/seam-20261022-114532-01aac4/`). Covered: opening holds and edges; confirmation by tap and by a resumed hold in persistent menus, including no selection; reorder by keys and chevrons; the Add page adding a discovered agent (8 found here) that then disappears from it; minimize robustness including an iconic window; the fast path; hub hover in both languages; draws at rest; cold, protocol and argument-less starts. |
| Rendering | `tools/capture-seam.py` compared Main, the hub with its selections and the edge-gated hold with the 0.3.4 captures in five variants (dark/light solid, contrast path, 96/144 DPI) and reported 0 differing pixels. **Correction (0.3.5.1):** that comparison used Pillow `getbbox()` on an RGBA difference, which only sees alpha, so colour differences were not counted, and the 0.35 s settle caught the hub selection seat still moving. Measured in every channel on the 0.3.5.1 build with a 1 s settle, every comparable still is within 2 levels per channel of 0.3.4 (see v0.3.5.1). New App actions frames at a middle, the first and the last position (`artifacts/qa/capture-seam-20261022-114552-32fb7d/`). |
| Discovery | `AgentPresets()` on this machine returns 11 rows: ChatGPT and Claude packages; Claude Science, ZCode, DeepSeek Harness, VS Code, Cursor (one row for two shortcuts) and Grok Bot by executable; empty Antigravity, JetBrains IDE and Junie rows. The Claude Code CLI and CC Switch are excluded. About 30-80 ms on a worker thread. |
| Review | Five independent review lenses (controller, discovery, renderer, test seam, product and docs) with adversarial verification. Confirmed findings were fixed: argument-less starts while the resident is starting or ready, a key released before the reel could open, shortcuts with their own arguments, editor-open state when the editor fails to open, catalog refresh under a selection, hung target windows, icon extraction during a gesture, chevron hit boxes overlapping the slot-0 tile, discovery at exit, synchronous discovery at cold start, staging-folder false negatives and weak test assertions. |
| Clean installation | 0.3.5.0 replaced 0.3.4.1 after a backup (`artifacts/backups/20261022-114645`); three configuration files were restored. Installed executable SHA-256 `2905D512…FA68` equals the tested build, all 42 packaged asset files match, the provider declaration is valid, the Copilot key selection is unchanged, and after the first start the startup task state is `2` (enabled). |
| ARM64 package | Cross-compiled with `tools/build.ps1 -Architecture arm64` (machine `0xAA64`, not run). `CopilotReel_0.3.5.0_arm64.msix` declares the startup task, is signed with the development certificate and passed `signtool verify /pa`; its executable SHA-256 is `5F305222…5BF2`. |

Not yet verified: a physical Copilot-key tap and hold in an open menu, at a cold start and after sign-in through the startup task; whether a running Cursor or DeepSeek Harness now comes forward; the real-input smoke in `docs/testing-plan.md`.

Known limits in 0.3.5:
- While the reel is not running, starting it from Start cannot be told apart from the Shell's fallback for a key press, so it opens the preferred agent. A separate visible Start entry with `--preview` and a hidden provider entry would remove this, but whether the Settings key picker still lists such a provider is unverified.
- A launcher stub whose window belongs to another executable (Claude Science) is started again instead of brought forward; packaged agents are activated again through `IApplicationActivationManager`.
- A saved absolute executable on a slow or disconnected network path is checked on the UI thread.
- A development reinstall turns the startup task back on; `tools/install-development.ps1` warns when it was off.

## Verified in v0.3.4.1

0.3.4.1 passes on the foreground right before each launch and shows Claude Science with its own mark. Selecting the already running DeepSeek Harness in 0.3.4.0 launched without error, but its window stayed behind ChatGPT.

| Area | Evidence and scope |
| --- | --- |
| Native build | MSVC `/W4` build and all unit tests passed; iconography now has 134 checks (an installed Claude Science executable or shortcut keeps its own Shell icon). |
| Packaging | After the ARM64 change, `tools/package.ps1` refused every x64 package because it treated the unchanged x64 manifest as missing its architecture token; it now checks for the token itself. The signed `CopilotReel_0.3.4.1_x64.msix` passed `signtool verify /pa`. |
| Clean installation | 0.3.4.1 replaced 0.3.4.0 after a backup (`artifacts/backups/20261020-211819`); three configuration files were restored. The installed executable SHA-256 `5EF65109…1F6C` equals the build, all 42 packaged asset files match, the provider declaration is valid, and the user's Copilot key selection is unchanged. |
| Artwork | A 192 DPI capture of the installed reel shows Claude Science's DNA mark beside the Claude mark. |
| Package context | A process started through ShellExecute from inside the Copilot Reel package has no package identity (`GetCurrentPackageFullName` returned `APPMODEL_ERROR_NO_PACKAGE`), so launched agents keep their own AppData. |
| ARM64 package | Cross-compiled with `tools/build.ps1 -Architecture arm64` (machine `0xAA64`, not run). `CopilotReel_0.3.4.1_arm64.msix` is signed with the same development certificate and passed `signtool verify /pa`; its executable SHA-256 is `776A7AAD…E219`. |

Not yet verified: whether a running target now comes forward after a physical Copilot-key hold. `run.log` records `Foreground grant unavailable` with the error code when the reel holds no foreground right at commit. `tests/run-radial.py`, language invariance and the capture suite were not rerun for this patch because they drive the pointer.

## Verified in v0.3.4

| Area | Evidence and scope |
| --- | --- |
| Native build and tokens | MSVC `/W4` build of the application, all unit tests and the launch probe passed. `tools/gen-vi-tokens.ps1 -Check` confirms `src/vi_tokens.h` matches `design/vi/tokens.json`. |
| Unit tests | Core (1-12 sectors, placement, ordering, routes; the hub and Manage keep a disabled Add slot at 8 apps), settings 48, behavior 65, iconography 132 (target-based identity including profile folders and look-alike hosts, artwork classes, plated tile clipping, centered glyph and Shell icons at 96 and 192 DPI), local launch 16, display names 51. |
| Native end-to-end input | `tests/run-radial.py` passed all 21 groups twice, including both native-picker groups: release 25-48 ms into the 167 ms opening cancels without launching; a cancel by `WM_CLOSE` and re-hold 25.7 ms into the 83 ms exit stays visible after 270 ms; launches remain exactly once. Evidence: `artifacts/qa/radial-20261020-185859-e515d3/`. |
| Placement | Interior anchors have 0.00 px center displacement; all eight edges and corners clamp minimally and arm only after center entry; no cursor warp. |
| Rendering at rest | Visible stationary surface draws stayed 51 to 51 and hidden draws 98 to 98 over 350 ms. Selection, hover and transitions never re-rasterize item artwork. |
| Shared language profile | `tests/run-language-invariance.py` passed twice: Main EN vs ZH and EN vs returned EN differ by 0 pixels; the hub differs by 0 pixels outside the EN/ZH tile. Chinese names render in Microsoft YaHei UI (no per-character JhengHei UI / Yu Gothic UI mix). Evidence: `artifacts/qa/language-invariance-20261020-185651-56cdbd/`. |
| Native visual states | `tools/capture-v034.py` captured Main idle, a vendor tile selected, the hub idle, a command selected, Language selected and an edge-gated hold for dark/light solid, dark/light frosted, the high-contrast code path, and 96/144/192 DPI into `output/runtime/v0.3.4/` (per-variant contact sheets `sheet-*.png`). The acrylic effect graph is created without error in both frosted variants. |
| Motion | Frame strips `motion-open-select.png`, `motion-drill.png` and `motion-commit.png` show the 83 ms fade with the 0.96 settle, the seat appearing and travelling along the orbit, the name crossfade with the ring giving way, drill-in with the tile handing off to the back chevron, drill-out, and the commit hold with exactly one probe launch. These are visual evidence, not frame-rate measurements. |
| Clean installation | The signed 0.3.4.0 package replaced 0.3.3.0 after a backup (`artifacts/backups/20261020-193438`); three configuration files were restored. The installed executable SHA-256 `B20F5456…5D47` equals the final build, all 42 packaged artwork/provenance/license files match source hashes, the provider declaration is valid, and the user's Copilot key selection is unchanged. |
| Post-review build | An independent code review led to fixes for capability events off the UI thread, a seat expression surviving a mid-travel hide, double-composited selected artwork, permanently cached failed Shell icon lookups, updates during the exit re-showing items, device-loss retry, and late launch-failure dialogs. On this final build (the installed executable) all unit tests pass, `tests/run-radial.py` passed all 21 groups again (`artifacts/qa/radial-20261020-193700-3d0b82/`, stationary draws 51 to 51, hidden 97 to 97), language invariance again showed 0 differing pixels (`artifacts/qa/language-invariance-20261020-193729-021589/`), and the captures in `output/runtime/v0.3.4/` were regenerated from it (`artifacts/qa/visual-v0.3.4-20261020-193740-82c3e9/`). |
| Official artwork | Plated marks (Claude, Perplexity, ZCode, DeepSeek Harness, Air) fill and are clipped to the 40 DIP tile; glyph marks share the per-brand optical scale; Windows Settings, Notepad and custom executables show their Shell icons; command glyphs come from Segoe Fluent Icons. Native contact sheets were reviewed at 96 and 192 DPI in light and dark. |

The captures use `COPILOT_REEL_VISUAL_OVERRIDE` and an isolated data directory. Window captures contain only the application HWND: the frosted captures do not show the DWM host backdrop or the desktop beneath, and `contrast=on` exercises the high-contrast code path with the current (non-high-contrast) system colors.

## Testing infrastructure review on 2026-10-20

The [appearance and performance testing plan](testing-plan.md) records the current coverage gaps, proposed injection seams, acceptance matrix, ETW/WPR reuse and implementation order. It is a plan, not additional application acceptance evidence.

- The local `C:\dev\lpt03\trace-code-bridge` 0.2.0 suite passed all 79 tests. Its existing .NET ETL exporter CLI loaded successfully. Synthetic `correlate` and `review` runs completed; their input explicitly identifies itself as artificial.
- Readiness evidence is retained in `artifacts/qa/trace-bridge-reuse-20261020-200016/`. No new WPR session, target instrumentation, GPU/frame measurement or system-setting change was performed.
- The reusable WPR workflow provides bounded recording, unique instances, phase markers and ETL checks. Its existing graphics profile and WPA views require Copilot-specific names, paths and process filters; no Copilot recorder wrapper has been implemented yet.
- The bridge exports CPU samples and stacks, not GPU or DWM frame metrics. Its tests and synthetic correlation do not establish Copilot rendering correctness or performance.
- Existing visual overrides are partial environment isolation: high-contrast colors and accents remain host-dependent, and a forced material can override the earlier energy-saver result. Deterministic policy/rendering tests and real-system integration remain distinct planned work.

The proposed unified Copilot confirmation model is also pending. Current 0.3.4 still changes to click/preview navigation after entering a submenu. The testing plan lists the required cross-input and page-transition regressions without marking them passed.

## ARM64 package, cross-compiled 2026-10-20

The x64 and ARM64 releases share one source tree. There is no architecture-specific input or rendering branch. This host is x64, so the ARM64 binaries were compiled with `tools/build.ps1 -Architecture arm64` and were not executed. The existing pointer-injection suite was not repeated; later x64 acceptance belongs to the [testing plan](testing-plan.md). The installed x64 0.3.4 app was left in place.

| Check | Result |
| --- | --- |
| Compiler | MSVC 14.44.35229 `Hostx64\arm64\cl.exe`, `/std:c++20 /W4 /O2 /MT`, Windows SDK 10.0.26100.0 ARM64 import libraries |
| Application | `build/arm64/Release/CopilotReel.exe`, `dumpbin` reports `AA64 machine (ARM64)`, PE32+, Windows GUI subsystem. SHA-256 `E9237A3132A361C61C94B7590B9A0051036BDEA5D0D38D775E2896A2A055DFA2` |
| Unit-test binaries | `core_tests`, `settings_tests`, `behavior_tests`, `iconography_tests`, `local_launch_tests`, `display_names_tests`, and `launch_probe` are all machine `0xAA64`. They were compiled and not started |
| Host execution | `CreateProcess` on `CopilotReel.exe` failed with "The specified executable is not a valid application for this OS platform." |
| Package | `artifacts/CopilotReel_0.3.4.0_arm64.msix`, signed with the development certificate `CN=CopilotReel Development` (`061ED5990CEBC4F2D7C252ABE3EFE7EB99D6239F`). `signtool verify /pa` succeeded. The staged manifest says `ProcessorArchitecture="arm64"`. `packaging/AppxManifest.xml` remains the x64 source manifest, and `artifacts/package` was not replaced |
| Installation | Not installed. `tools/install-development.ps1` rejects any package whose architecture is not x64 |

The signature matches the x64 development package. Installation and Copilot-key registration remain an ARM64-machine step. Acrylic, high contrast, physical Copilot-key timing, and the unit-test assertions are unchecked on ARM64.

## Verified in v0.3.3

| Area | Evidence and scope |
| --- | --- |
| Native build | C++20 Win32 / Windows Composition / Direct2D / DirectWrite build passed with MSVC `/W4`. |
| Core geometry and state | 1-12 sectors, 96-384 DPI geometry, negative monitor origins, eight boundary positions, center-entry gating, delayed Down/Up ordering, exactly-once release, radial action routing, preset pagination, and protocol parsing passed. |
| Configuration | 48 parser/validation checks passed. Invalid data is preserved; fallback does not overwrite it. |
| Preferred profile | 65 checks passed, including rename/reorder stability, deleted preference fallback, data-directory isolation, UTF-16 BOM, corrupt preferences, and atomic replacement failure. |
| Native end-to-end input | `tests/run-radial.py` passed 19 interaction groups; the two native-picker groups passed in a focused rerun after correcting a fixture race with dialog initialization. The application picker code was unchanged. Cold/warm Tap, cold/warm Down, stop, duplicate Down/Up, orphan cold Up, protocol Tap, and the registered message contract are covered using harmless launch probes. |
| Animation interruption | Release 25.79 ms into the 160 ms opening canceled without launching; reopening 18.3 ms into the 120 ms exit remained visible after 270 ms. The old close timer did not hide the new gesture. |
| Missing preset | Selecting unresolved Claude Science opened the focused editor with its official name and an empty target. Cancel preserved the configuration byte-for-byte and launched nothing. |
| Actual pointer placement | On this 200% DPI display, the canvas measured 768 pixels. Interior anchors (1440, 2700) and (912, 2958) had zero center displacement. All eight edge/corner positions used only the minimum translation and stayed inside the tested monitor work area [0, 1800, 2880, 3600]. Tests verified the captured pointer and that the application did not warp it. |
| Radial navigation | Main has one + entry, opening a combined hub for Language, Appearance, Preferred, Manage, and Add. Both preset invocation paths return to their recorded parent. Entry actions, pagination, custom editor, and cancellation back to Manage passed. Center and Escape returned one level. |
| Functional hover labels | Twenty-six observations verified real mouse and keyboard selection across English/Chinese hub actions, both language arrows, main app/+ items, and idle clearing. One name is rendered; no explanatory second line is added. |
| Local short tap | Native picker cancellation preserves data and launches nothing. Accepting the harmless executable saves the profile, retains a backup, launches once, and makes the next Tap direct. Sixteen shortcut-verification checks reject URL/missing targets without execution. Exact legacy ChatGPT/Claude homepage migrations preserve display names and preferred identity. Installed-user profiles now point to their native AUMIDs; the preferred index remains ChatGPT. |
| Built-in display names | 51 checks passed across English, Chinese, and English again. Historical Notepad/Windows Settings names translate only for display, including in the focused editor. Names, targets, custom labels, and launch identity remain intact. |
| Shared language profile | One native instance used one isolated apps.ini and behavior.ini while switching EN to ZH to EN. Both files retained identical SHA-256 hashes, app order and preference remained fixed, and all captures used the same dataPath. |
| Pixel-level icon invariance | Main EN versus ZH: zero differing pixels. Main EN versus returned EN: zero. Hub outside the EN/ZH tile: zero. Hub EN versus returned EN: zero. Actual captures are in output/runtime/v0.3.3/. System hover names show the appropriate language; only the locale monogram changes in the idle hub. |
| Native language input | Actual `SendInput` A/D, the EN/ZH tile, and both arrows beside that tile switched and persisted English/Chinese. Testing found Chinese IME interception; only the command HWND now disables IME. Text editors retain IME input. |
| Rendering work at rest | Stationary visible surface draws stayed 88 to 88 over 350 ms; hidden draws stayed 216 to 216 over 350 ms. This verifies absence of repeated surface rasterization in those observations, not GPU cost or high-refresh presentation quality. |
| Official artwork | All 39 artwork hashes match provenance. Thirty-five icon checks passed: strict target-based Windows Settings/Notepad glyphs, nonempty/distinct WIC renders, and SVG-capable D3D11/WARP bounds checks. Gemini and Perplexity use official artwork. Perplexity physical-unit SVG dimensions are normalized in the cached document so no pixels escape its optical bounds; original source files remain unchanged. |
| Actual wheel images | Windows Graphics Capture captured the application HWND at 192 DPI, independently of overlapping desktop windows. Main and combined-hub images show the six default identities, EN/ZH, functional hover names, matching geometry, and no renderer error. See `output/runtime/v0.3.2/`. The unchanged motion implementation was captured in v0.3.1; that sequence is visual evidence, not a frame-rate benchmark. This validates the tested dark, opaque setup. |
| VI | v0.8 PDF contains 12 English pages, all rendered and visually inspected. The active vector masters describe the single + entry, one-line functional hover names, EN/ZH and sun/moon state, zero interior pointer offset, and restrained native motion. Evidence: `artifacts/qa/vi-v08/results.json`. |
| Clean installation | Old 0.3.2.0 package was removed, signed 0.3.3.0 installed, and four existing configuration files across the installed/development scopes were restored byte-for-byte. This release does not rewrite saved names when language changes. Installed executable SHA-256 equals the final tested build. All 42 packaged artwork/provenance/license files match source hashes. |
| Copilot registration | Installed provider and all three state declarations are valid. AUMID remains `CopilotReel.Desktop_nks6vs6sjb1qw!App`. The user's current Windows selection is this app. No tool wrote the key preference. |

Controller evidence: `artifacts/qa/radial-20261020-132622-600967/results.json` (19 groups, then a picker-fixture timeout).
Focused picker rerun: `artifacts/qa/radial-20261020-132834-3f2e1d/results.json` (both picker groups passed).
Language/image invariance: `artifacts/qa/language-invariance-20261020-132456-0e03b9/results.json`.
Native motion evidence: `artifacts/qa/quiet-motion/capture-results.json`.
Installation receipt: `artifacts/installation.json`.
Configuration backup: `artifacts/backups/20261020-132908`.
These local evidence and signing directories are ignored by Git. Private signing material is never packaged.

The prior Windows Settings UI Automation check observed a visible, enabled Copilot Reel item. The new package retains that provider identity. Manifest verification and the saved current selection are not a new UI Automation observation of an open picker.

## Remaining checks and known limits

- Real physical Copilot short-tap/hold/release delivery, including hidden-window discovery and pathological Shell scheduling. Automated commands and messages do not establish hardware timing.
- Presentation/resource measurements at 60/144/240 Hz, mixed refresh rates, battery/energy saver, and a longer idle baseline. Compositor animation alone does not guarantee smoothness on every GPU.
- Live frosted-backdrop appearance over real content (this machine has Windows transparency effects off), real high-contrast themes, reduced motion, device loss, mixed-DPI multi-monitor transitions, and small-work-area usability.
- Known limits in 0.3.4: during the 83-133 ms exit a click over the plate is ignored but not passed to the application beneath (`HTTRANSPARENT` forwards only within the thread); retargeting the seat twice within one spring settle measures the shortest arc from the previous target, so a 180-degree flick can swing the long way round.
- Cold, running, and minimized instances of every preset application. Discovery is verified on this machine for ChatGPT/Codex, Claude, ZCode, and VS Code; the remaining presets ask for their actual installed target.
- Comprehensive accessibility/UI Automation semantics for custom radial controls. The focused text editor uses native controls.
- ARM64 runtime. The 2026-10-20 package is a native ARM64 image checked by its PE header and by this x64 host refusing to start it. Behavior, appearance, and Copilot-key registration on ARM64 hardware are still open.

## Repeatable checks

```powershell
.\tools\build.ps1
python .\tests\run-radial.py
uv run --python 3.14 --with pillow python .\tests\run-language-invariance.py
uv run --python 3.14 --with pillow python .\tools\capture-v034.py
.\tools\verify-registration.ps1
```

The radial integration fixture requires no resident Copilot Reel instance, creates a fresh isolated directory, launches only the harmless probe, and restores the pointer. Avoid desktop interaction while it runs. Historical v0.2 tests are archived in `tests/legacy-v0.2/`; they do not describe current UI acceptance.

`tools/install-development.ps1 -PackagePath <signed-msix>` validates signature, identity, and tested executable before removing the exact old package. It shuts down safely, backs up configuration, performs uninstall/install, restores verified data, and checks installed binaries/assets. It does not import certificates or change the Copilot target.

`tools/build-capture-tool.ps1` builds a standalone Windows Graphics Capture helper. `tools/capture-native-wheels.py` captures only the explicitly selected application window using isolated demo entries, even when another desktop window overlaps it. These are actual native renders, not PDF design scenes. Desktop screen captures were rejected when occlusion produced blank images.
