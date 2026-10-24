# Agent handoff

Copilot Reel is developed by several coding agents in turn. Before starting, read this file, `git log` and `git status`. Before handing over, add your session and what it changed. The documents in `docs/` remain the product and design contract; this file records who did what and the rules every agent follows.

## Rules for every agent

- Development artifacts are English: code, comments, script output, docs, design tokens, VI and commit messages. Replies to the user are in Chinese.
- The product UI defaults to English, with Simplified Chinese (`ZH`) as the option. Both languages share one profile: switching language never changes apps, order, icons or the preferred agent.
- After every push to GitHub, release or not, reinstall Copilot Reel on this machine from the pushed commit (the user's rule since 2026-10-23): build Release x64, package it, sign it with `signtool sign /fd SHA256 /f artifacts\signing\CopilotReel-Development.pfx <msix>`, then `tools/install-development.ps1 -PackagePath <signed msix>`, which backs up the configuration, uninstalls the old package, installs the new one and restores the configuration. Then start it with `tools/open-installed.ps1 -View Background` and check that the installed executable's SHA-256 equals `build\Release\CopilotReel.exe`. Skip the reinstall only when that hash already matches.
- Only the newest version is kept (the user's rule since 2026-10-22): after a release is published, delete the older GitHub releases together with their tags (`gh release delete <tag> --cleanup-tag`, then `git tag -d <tag>`), and remove the older packages and release notes from `artifacts/`. The commits stay in history; configuration backups in `artifacts/backups/` are kept.
- Commits use the existing author, `baka_sayaka <60833924+bakasayaka@users.noreply.github.com>`, passed per command. Never change git config. Commit or push only when the user asks.
- Interaction and visual checks run through the test seam: `tests/run-reel-seam.py` and `tools/capture-seam.py` drive a separate unpackaged test instance through its own channel. They never move the pointer, send input or take the foreground, and the installed reel keeps running.
- `tests/run-radial.py`, `tests/run-language-invariance.py` and `tools/capture-v034.py` are 0.3.4 evidence, and `tools/capture-native-wheels.py` is older evidence; all of them assume the five-item hub with Preferred agent and the 384 DIP window size. They drive the real pointer, need the installed instance to quit first and must not run while the user works. Restart the installed reel afterwards with `tools/open-installed.ps1 -View Background`.
- Settled decisions; do not reopen them without the user:
  - Soft Orbit with one plate.
  - A quiet center: one name on hover, no titles or captions.
  - EN/ZH and sun/moon show state in the icon itself.
  - One merged `+` entry.
  - Official vendor artwork with one tile treatment.
  - The reel opens centered on the pointer and moves only as far as needed at screen edges. Selection waits for center entry only when that move leaves the pointer outside the plate; a pointer still on the plate selects by direction as soon as it moves (changed 2026-10-22).
  - No line, perimeter-sweep or accent-stroke opening animation.
  - While the reel is closed, a short tap opens the preferred local agent, never a web page; a hold opens the reel.
  - Inside an open menu every input confirms, goes back or adjusts: a tap, or a new hold and its release, confirms the selected item (approved 2026-10-21).
  - The reel starts in the background at sign-in through a packaged startup task (approved 2026-10-21).
  - The reel draws at 0.8 of its DIP geometry (`geometry.scale`; requested 2026-10-23).
  - Apps are reordered only by dragging their tiles on Manage apps; while a tile is dragged the center shows a red trash, and dropping on it takes the app out of the reel without deleting it: its settings wait first on Add. App actions has no reorder chevrons (requested 2026-10-23).
  - The preferred agent is set only from an app's actions page; the hub has no Preferred agent entry (requested 2026-10-23).

## Sessions

| Tool | Session ID | Transcript |
| --- | --- | --- |
| Codex | `01a15515-8bd6-7c13-b974-98614ac37e54` | `%USERPROFILE%\.codex\sessions\2026\10\19\rollout-2026-10-19T18-53-57-01a15515-8bd6-7c13-b974-98614ac37e54.jsonl` |
| Claude Code | `f4bd2732-bc2e-40e3-bdc3-52ecd97171f3` | `%USERPROFILE%\.claude\projects\c--dev-copilot-reel\f4bd2732-bc2e-40e3-bdc3-52ecd97171f3.jsonl` |
| Claude Code (0.3.6) | `4cf620db-e965-4178-9bd4-a5b67a0a85a4` | `%USERPROFILE%\.claude\projects\C--dev-copilot-reel\4cf620db-e965-4178-9bd4-a5b67a0a85a4.jsonl` |
| Cursor | `dc23e9cc-f13b-4052-819b-5fa60d746ad3` | `%USERPROFILE%\.cursor\projects\c-dev-copilot-reel\agent-transcripts\dc23e9cc-f13b-4052-819b-5fa60d746ad3\` |
| Claude Code (CreeperUX fork) | `7da4ef51-28c5-4f30-a436-de72a2e32478` | `%USERPROFILE%\.claude\projects\C--Users-Creep-Documents-Claude-Workspace-Lumen\7da4ef51-28c5-4f30-a436-de72a2e32478.jsonl` |

### Codex: product, platform and interaction

Role: platform research, product behavior, input and launch logic, and test strategy. Codex started the project and owns how the reel behaves.

Work, 2026-10-19 to 2026-10-20:
- Reverse-engineered how Windows Settings lists Copilot key providers (REA and Ghidra on `SettingsHandlers_nt.dll`, 184 package manifests). It chose the official provider contract with the roughly 300 ms hold over a keyboard hook (`docs/research.md`).
- Created the repository. Built the native C++20 Win32 provider with the URI and HWND fast paths, MSIX packaging and the development signing certificate, and confirmed that Windows Settings lists Copilot Reel.
- Took the product from the first Soft Orbit prototype to 0.3.3, and the VI from v0.1 to v0.8:
  - Pointer-centered placement with minimal edge clamping.
  - The radial settings hub behind one `+`.
  - Official agent presets.
  - Short tap and hold behavior, with local-first launching.
  - EN/ZH localization with pixel-identical icons.
- Wrote `docs/testing-plan.md`: appearance isolation from the live system, reuse of the ETW/WPR workspace for performance, and a proposed unified Confirm / Back / Adjust interaction model based on ASUS OSD and macOS menus. None of it is implemented yet.
- Commits: `b44c275`, `bde6c85`, `cf9eb0c`, `4c1341d`, `dc4da31`, `a298c96`, `2f9e1f2`, `92966d7`.

### Claude Code: visual design, rendering and motion

Role: art direction and its implementation (VI, design tokens, renderer, iconography, motion), code review, and local configuration on this machine.

Work, 2026-10-20:
- Audited the VI against the code and rebuilt the renderer for 0.3.4:
  - a layered composition tree on Windows neutral plates with one flyout shadow and an acrylic graph;
  - a selection seat that travels along the orbit;
  - drill-in/out transitions in which the tapped tile becomes the back chevron;
  - plated, glyph and command artwork classes identified by launch target;
  - the live accent ramp and a YaHei UI fallback for Chinese text.
- Made `design/vi/tokens.json` the single source of design values. `tools/gen-vi-tokens.ps1` generates `src/vi_tokens.h` from it, and the VI generator reads it directly. Published VI v0.9 (15 pages).
- Added native capture tooling (`tools/capture-v034.py`, `capture-window.exe`). Fixed the findings of an independent code review, passed the regression, and installed 0.3.4.
- Replaced Gemini, Perplexity and Notepad on this machine's reel with Cursor, ZCode and DeepSeek Harness, added Claude Science after Claude (see [This machine](#this-machine)), and wrote this file.
- Released 0.3.4.1 as the prerelease `v0.3.4.1-beta` and installed it here:
  - the reel passes on its foreground right before a launch, so an already running agent should come forward instead of flashing in the taskbar;
  - an installed Claude Science shows its own mark instead of the Claude mark;
  - `tools/package.ps1` packages x64 again; the ARM64 change had made it refuse every x64 package.
- Commits: `47a63e9`, `2bc3534`, `d905a41`, `eac04da`.

Work, 2026-10-21 to 2026-10-22 (0.3.5, released as the prerelease `v0.3.5-beta` and installed here):
- Traced two field defects to the Shell (`twinui.pcshell.dll`): it starts a provider that is not running with no arguments, and minimizes the provider window after a fast-path tap. Argument-less starts are now classified (startup task, held key, ready resident, cold tap), a packaged startup task keeps the reel resident from sign-in, and the reel ignores minimize commands.
- Implemented the unified Confirm / Back / Adjust model the user approved: `ReelInput` (`src/core.h`) and `RouteKey` (`src/radial_menu.h`).
- Local agent discovery (`src/agent_catalog.h`, `src/presets.cpp`, `InstalledIdentity`): catalog rules by executable and install folder from Start-menu shortcuts, Uninstall keys and folder probes, on a worker thread. Add lists installed agents not yet in the reel first, one tap adds them.
- Reorder from an app's actions page with per-side chevrons (`src/renderer.cpp`) or A/D.
- A running desktop agent comes forward instead of starting again (`ActivateRunning`).
- The test-input seam (`src/test_input.*`), `tests/run-reel-seam.py` and `tools/capture-seam.py`: interaction and pixel checks without touching the pointer, input, focus or the installed reel.
- A five-lens review with adversarial verification; all confirmed findings fixed. Evidence and limits are in `docs/validation.md` under v0.3.5.

Work, 2026-10-22 (0.3.5.1, released as `v0.3.5.1-beta` and installed here):
- Fixed the Add page artwork the user reported (JetBrains IDE and Junie showed Antigravity's mark): the renderer now caches tile artwork by `ArtworkIdentity`, and `tools/capture-seam.py` checks every Add tile against its identity across pages. The 0.3.5 screenshot comparison had only compared alpha; it now compares every channel.
- Edge gating at the plate radius, at the user's request; the opening position never selects.
- Packaged agents and launcher stubs are brought forward; network targets are never touched on the UI thread; a reinstall restores a turned-off startup task; `ParseUri` accepts `&inputTimestamp=`; a cold start from Start or search opens the preview.
- Reverse-engineered why the provider cannot be hidden behind a separate Start entry (`docs/validation.md`, v0.3.5.1).

Work, 2026-10-23 (0.3.6, session `4cf620db`, released as the prerelease `v0.3.6-beta` and installed here):
- Renamed the GitHub repository to `bakasayaka/copilot-reel` and pointed `origin` at it; the local folder is still `C:\dev\copilot_reel`.
- The reel draws at 0.8 of its DIP geometry: `geometry.scale` / `vi::kReelScale`, applied as a whole effective DPI (`ReelDpi` in `src/main.cpp`), so placement, hit testing, surfaces and artwork stay on one pixel grid and the seam harness needs no geometry change.
- Drag to reorder on Manage apps (`Controller::Press`/`DragPointer`/`Drop`, `DropSlot`, `MoveEntry` from/to), with a red center trash (`critical` palette roles, `geometry.trash`) that parks an app in `parked.ini` (`Park`, `LoadParked`/`SaveParked`, a backup before an unreadable list is rewritten). Add lists parked apps first and `MenuAction::Restore` puts one back as saved. A/D move and Delete parks the selected app; Remove from reel parks too. Renderer: `BeginDrag`/`DragTo`/`EndDrag` and `PageTransition::Reflow`, which starts each tile where it is drawn.
- Removed the App actions reorder chevrons and the hub's Preferred agent page (`MenuPage::Entry` keeps 5); Set as preferred returns to Manage apps.
- Seam: `TestAction::Press`/`Release`, group `c` rewritten, `tools/capture-seam.py` captures the drag and trash and compares with the new `output/runtime/v0.3.6` references. A five-lens review with adversarial verification; all confirmed findings fixed (`docs/validation.md`, v0.3.6).

Work, 2026-10-23 (0.3.6.1, same session, released as the prerelease `v0.3.6.1-beta` and installed here):
- Fixed the reel opening beneath other windows after a background start: Windows silently ignores `SetWindowPos(HWND_TOPMOST)` for a process without foreground rights, so the window is now created with `WS_EX_TOPMOST` (`docs/validation.md`, v0.3.6.1).

### Cursor: build, release and distribution

Role: toolchain, multi-architecture builds, signing, the GitHub remote and releases.

Work, 2026-10-20:
- Added an ARM64 release that is cross-compiled from the same source with the pinned MSVC 14.44 and the Windows SDK 10.0.26100 ARM64 packages:
  - `tools/build.ps1 -Architecture arm64` and `tools/package.ps1 -Architecture arm64` produce it, and `tools/toolchain-manifest.json` pins the toolchain;
  - `tools/install-development.ps1` accepts only x64 packages;
  - ARM64 binaries are checked for PE machine `0xAA64` and never run on this x64 host.
- Ran the x64 regression: unit tests, 21/21 radial groups, and 0 differing pixels between languages. Created the private repository <https://github.com/bakasayaka/copilot-reel> (renamed from `copilot_reel` on 2026-10-23) and pushed `main`.
- Published the prerelease `v0.3.4-beta`. It carries both packages, signed with the development certificate `CN=CopilotReel Development` (thumbprint `061ED5990CEBC4F2D7C252ABE3EFE7EB99D6239F`). The ARM64 package was signed after the release was created and replaced on it. The matching `docs/validation.md` update was left uncommitted.
- Commit: `b6fb4e7`.

### Claude Code (CreeperUX fork): installed-app picker

Role: a contribution from a fork, `CreeperUX/copilot-reel`, offered to `bakasayaka/copilot-reel` as a pull request. It ran on another machine, an ARM64 Windows 11 PC (NVIDIA RTX Spark N1X), with its own self-signed `CN=CopilotReel Development` certificate; the AGENTS.md reinstall rule and [This machine](#this-machine) describe the maintainer's x64 PC, not this one. Commits use the contributor's own author.

Work, 2026-10-24 (branch `feature/installed-app-picker`):
- The user asked to pick an app from what Windows has registered instead of typing a path. The editor gained **Choose app…** beside the target: a page that lists Start's All apps (`src/installed_apps.*`, `InstalledApps`) with their Start icons and a search box. Packages store their AUMID; desktop apps store their Start shortcut's executable and arguments, so both pass the short tap's local-application check (`StartEntryApp`). Websites, documents, MMC consoles, shell locations and uninstallers are left out. **Browse…** and the editable target stay.
- The list is read on a worker thread once per editor window; icons follow one message each.
- Audited the focused editor's pages from captures at 200% in both languages and fixed their layout: rounded, vertically centered text fields with a focus ring (painted frames around borderless EDIT controls), one DIP grid for every page, square header buttons, even gaps, Advanced in the footer, whole list rows, dark list scrollbars, empty and loading messages, a disabled primary button that reads as disabled, and Up/Down from the search box (`docs/validation.md`). The quick-menu pages (Home, Apps, Appearance, Settings, About) and `ShowSettings` in `src/settings.cpp` are never opened by the product and were left as they are.
- `tests/installed_apps_tests.cpp` covers the classification with entries recorded from this machine's Start, and checks that every app listed here passes `IsVerifiedLocalApplication`.
- First ARM64 run on ARM hardware: the ARM64 package is installed and runs on this machine (`docs/validation.md`).
- The tray icon's menu was a plain popup menu that stayed light on a dark desktop. `ShowTrayMenu` (`src/tray_menu.*`) sets uxtheme's preferred app mode from the reel's appearance (also on every appearance change, so the editor's context menus follow), adds Segoe Fluent glyphs (`iconography.commandGlyphs.Tray*`), makes Preview the bold default and opens from the icon's anchor away from the taskbar. Checked with a scratch harness in both themes and languages; not yet from a real right-click in the installed package.
- Touchpad scrolling in the Choose app list jumped a whole row at a time, because a LISTBOX scrolls only by rows. The LISTBOX now sits, as tall as its rows, inside a `CopilotReel.ListViewport` that scrolls it by pixels in proportion to each wheel delta (`ScrollListTo`, `LayoutList`, `WheelList`, `ListProc` in `src/osd.cpp`); it keeps selection, keys and accessibility. Measured with a message-only harness; the physical touchpad still has to confirm it.
- The user found the reel too large on a 14-inch screen and asked for size steps. The hub has a fifth item, Reel size (`MenuAction::Size`, `reel:size`), between Appearance and Manage apps: 70%, 85%, 100% and 115% of the 0.8 scale (`geometry.sizeSteps`), shown in the tile and stored as `ReelSize` in `preferences.ini` (`LoadReelSize`/`SaveReelSize` in `src/localization.cpp`). `ReelScale()` folds the step into the effective DPI, and `PlaceReel` (split out of `Start`) places the resized reel again, so it never leaves the work area. Checked through the seam with a C++ client; `tests/run-reel-seam.py` is updated but was not run (no Python here).

## Current state

- 0.3.6.1 x64 is installed on this machine and runs in the background; the Copilot key is bound to it. GitHub carries only the prerelease `v0.3.6.1-beta`, with signed x64 and ARM64 packages; older releases and tags were removed under the keep-the-newest rule.
- `main` tracks `origin/main` on GitHub, now `bakasayaka/copilot-reel` (GitHub reports the repository as public). The ARM64 package has never run on ARM hardware. The development certificate's PFX in `artifacts/signing/` has no password: `signtool sign /fd SHA256 /f artifacts\signing\CopilotReel-Development.pfx <msix>`.
- Open validation items are listed in `docs/validation.md`. The physical Copilot key still has to confirm: tap and hold inside an open menu, a cold start, the start after sign-in, a cold start from Start (`foreground=` in `run.log`), and running agents coming forward.
- VI v0.9 is not regenerated for 0.3.5-0.3.6: it has no App actions or drag scene, still draws the hub with Preferred agent and a Preferred page (`design/vi/source/reel.py`), and draws the Claude Science preset with the Claude mark. The tokens and docs already describe the 0.8 scale, the trash and the hub, which the fork made five items with Reel size.
- A real mouse drag (capture, `WM_CAPTURECHANGED`, the system drag threshold) has only been exercised through the seam, which skips capture; it needs one check by hand.

## Open product questions

- Start launch while the reel is not running: the activation is identical to the Shell's argument-less fallback for a key press. A hidden provider entry with a separate Start entry is ruled out: the Settings Copilot-key picker skips providers whose application has `AppListEntry="none"` (reverse-engineered on 2026-10-22). 0.3.5.1 treats a cold start as a preview when Start or search had the foreground (not the taskbar: it stays in front after Win+T or a taskbar click, where a key tap must still open the preferred agent); whether Start is still in front when the process starts needs one physical check (`run.log` logs `foreground=`).
- Why the Shell sometimes has no cached SingleTap URI and activates the provider without arguments is unknown; nothing in the manifest selects that branch.
- Catalog rules confirmed only from vendor docs or third-party sources: Windsurf/Devin, Kiro, Trae, Trae CN, Antigravity, JetBrains Toolbox folder names. JetBrains Air and Junie are not discovered.

## This machine

The installed reel is configured in `%LOCALAPPDATA%\Packages\CopilotReel.Desktop_nks6vs6sjb1qw\LocalCache\Local\CopilotReel\`. Its order is:

1. ChatGPT (preferred)
2. Claude
3. Claude Science
4. Cursor
5. ZCode
6. DeepSeek Harness
7. Windows Settings
8. `+`

Claude Science, Cursor, ZCode and DeepSeek Harness point at their executables under `%LOCALAPPDATA%\Programs`. The configuration from before these changes, with web targets for Gemini and Perplexity and with Notepad, is backed up in `artifacts/backups/20261020-210309`.
