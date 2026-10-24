"""Seam-based interaction acceptance for Copilot Reel on x64.

Every group drives its own unpackaged test instance of the Release binary through the
interaction test seam (src/test_input.h, tests/reel_seam.py) with isolated data and the
harmless launch probe. The suite never moves the pointer, sends input, takes focus, or
quits or restarts the installed reel, so it runs while the installed instance stays
resident. Groups that open the native editor or file picker take focus and are not part
of this quiet tier; they, and the real-input checks listed in docs/testing-plan.md, stay
separate.

    uv run --python 3.14 python tests\\run-reel-seam.py [--exe build\\Release\\CopilotReel.exe] [--groups a,b,...]
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DRIVER = HERE / 'reel_seam.py'
# Spelled in parts so this file passes its own check.
FORBIDDEN = tuple(a + b for a, b in (('Send', 'Input'), ('SetCursor', 'Pos'), ('mouse', '_event'), ('keybd', '_event')))


def static_self_check():
    """Refuse to run before any driver code executes if either source names an input-injection API."""
    found = [f'{path.name}: {token}' for path in (Path(__file__).resolve(), DRIVER)
             for token in FORBIDDEN if token in path.read_text(encoding='utf-8')]
    if found:
        raise SystemExit('Refusing to run: the seam suite must not inject input (' + ', '.join(found) + ').')
    return [str(Path(__file__).resolve()), str(DRIVER)]


CHECKED_SOURCES = static_self_check()
_spec = importlib.util.spec_from_file_location('reel_seam', DRIVER)
seam = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(seam)
assert seam.forbidden_tokens() == FORBIDDEN

APPS = ['Probe 0', 'Probe 1', 'Probe 2']
PLUS = 'Apps and settings'
LANGUAGE, APPEARANCE, SIZE, MANAGE, ADD = 'Language', 'Appearance', 'Reel size', 'Manage apps', 'Add app'
HUB = [LANGUAGE, APPEARANCE, SIZE, MANAGE, ADD]
HUB_ZH = ['语言', '外观', '轮盘大小', '管理应用', '添加应用']
CUSTOM, PREVIOUS, NEXT = 'Custom app', 'Previous', 'Next'
EDIT, SET_PREFERRED, REMOVE = 'Edit app', 'Set as preferred', 'Remove from reel'


class Evidence:
    def __init__(self):
        self.placements, self.interruptions, self.hover, self.draws, self.foreground = [], [], [], [], []
        self.add = []


class Group:
    """One group: its own data directory, run id, probe apps and test instance."""

    def __init__(self, name, index):
        self.name = name
        self.data = DATA / name
        self.run_id = f'{SESSION}-{index:02d}'
        self.checks, self.notes = [], []
        self.data.mkdir(parents=True)
        # Arguments keep every probe on the launch path: only targets without them can match a running window.
        seam.write_apps(self.data / 'apps.ini', [(app, PROBE, f'--radial-probe={i}') for i, app in enumerate(APPS)])
        self.reel = seam.Reel(EXE, self.data, self.run_id, pointer=ANCHOR, visual=ARGS.visual)

    def passed(self, text):
        self.checks.append(text)

    def note(self, text):
        """A check this host could not exercise, recorded instead of passed."""
        self.notes.append(text)


# Shared assertions ------------------------------------------------------------------------
def verify_placement(g, offset, point, held):
    reel = g.reel
    match = seam.wait(lambda: seam.OPENING.search(reel.read_log()[offset:]), 'placement evidence')
    seam.wait(reel.visible, 'visible reel')
    assert match.group(1) == ('Hold started' if held else 'Preview opened'), match.group(0)
    pointer_x, pointer_y, left, top, right, bottom = map(int, match.groups()[1:7])
    assert (pointer_x, pointer_y) == tuple(point), 'The opening must use the delivered synthetic pointer.'
    assert (left, top, right, bottom) == reel.rect(), 'The logged placement must be the actual window rect.'
    work = seam.work_area(point)
    assert work[0] <= left < right <= work[2] and work[1] <= top < bottom <= work[3], 'The full plate must stay on its monitor.'
    size = right - left
    assert size == bottom - top, 'The circular canvas keeps its aspect ratio.'
    expected_left = max(work[0], min(point[0] - size // 2, work[2] - size))
    expected_top = max(work[1], min(point[1] - size // 2, work[3] - size))
    assert (left, top) == (expected_left, expected_top), 'Only a work-area boundary may displace the reel, and only minimally.'
    x, y = reel.center()
    distance = math.hypot(x - point[0], y - point[1])
    if (expected_left, expected_top) == (point[0] - size // 2, point[1] - size // 2):
        assert abs(x - point[0]) <= .5 and abs(y - point[1]) <= .5, 'An unconstrained opening is centered on the pointer.'
    guarded = held and distance > seam.PLATE_RADIUS_DIP * reel.scale()
    assert match.group(8) == ('true' if guarded else 'false'), \
        'Only a hold the clamp leaves outside the plate waits for center entry.'
    EVIDENCE.placements.append({'group': g.name, 'pointer': list(point), 'bounds': [left, top, right, bottom],
                                'centerDisplacementPixels': round(distance, 2),
                                'centerDisplacementDip': round(distance / reel.scale(), 2), 'awaitingCenter': guarded,
                                'held': held, 'dpi': seam.user32.GetDpiForWindow(reel.hwnd)})
    return guarded


def open_hold(g, point, via='command'):
    """A Copilot hold at a synthetic point. The cursor moves there first, as a real hold implies."""
    reel = g.reel
    point = (round(point[0]), round(point[1]))
    reel.pointer(point)
    offset = len(reel.read_log())
    if via == 'fast':
        reel.copilot(seam.FAST_HOLD_START)
    else:
        assert reel.send_command(seam.HOLD_START, point), 'HoldStart was refused.'
    return verify_placement(g, offset, point, True)


def open_preview(g, point):
    reel = g.reel
    point = (round(point[0]), round(point[1]))
    reel.pointer(point)
    offset = len(reel.read_log())
    assert reel.send_command(seam.PREVIEW, point), 'Preview was refused.'
    verify_placement(g, offset, point, False)
    state = reel.diagnostics()
    assert state.flag('preview') and state.flag('activationRequested'), 'A preview asks for activation, which the seam records.'
    assert_quiet(g)


def release(g, via='command'):
    if via == 'fast':
        g.reel.copilot(seam.FAST_HOLD_STOP)
    else:
        assert g.reel.send_command(seam.HOLD_STOP), 'HoldStop was refused.'
    g.reel.sync()


def assert_launch(g, expected, name):
    reel = g.reel
    seam.wait(lambda: reel.launches() >= expected, 'the harmless launch probe for ' + name)
    time.sleep(.1)
    assert reel.launches() == expected, f'One action must dispatch exactly one launch ({reel.launches()} != {expected}).'
    targets = reel.launched_targets()
    assert len(targets) == expected and targets[-1] == name, targets
    assert reel.launch_arguments()[-1] == '--radial-probe=' + name.split()[-1], reel.launch_arguments()


def assert_no_launch(g, expected):
    g.reel.sync()
    time.sleep(.15)
    assert g.reel.launches() == expected and len(g.reel.launched_targets()) == expected, 'Nothing may launch here.'


def assert_page(g, page, title=None, names=None):
    state = g.reel.diagnostics()
    assert state.page == page and state.flag('gestureActive'), state.text
    if title is not None:
        assert state.title == title, (title, state.title)
    if names is not None:
        assert state.names == names, (names, state.names)
    return state


def assert_quiet(g):
    """The test instance never owns the foreground and real input never reaches it."""
    reel = g.reel
    if reel.process is None or reel.process.poll() is not None:
        return
    _, pid = seam.foreground()
    assert pid != reel.process.pid, 'A test instance must never take the foreground.'
    EVIDENCE.foreground.append({'group': g.name, 'foregroundPid': pid, 'testPid': reel.process.pid})


def center_label(g, expected, source):
    state = g.reel.diagnostics()
    assert state['centerLabel'] == expected, (source, expected, state['centerLabel'])
    EVIDENCE.hover.append({'group': g.name, 'source': source, 'label': state['centerLabel']})
    return state


def language(g):
    path = g.data / 'preferences.ini'
    return path.read_text(encoding='utf-16') if path.exists() else ''


def wait_language(g, code):
    seam.wait(lambda: 'Language=' + code in language(g), 'language preference ' + code)


def nudge(reel):
    """An interior point other than the exact center, so an unmoved-cursor rule cannot keep a selection."""
    x, y = reel.center_point()
    return x + max(4, round(10 * reel.scale())), y


def interior_points(size):
    left, top, right, bottom = seam.work_area(ANCHOR)
    return [(round(left + size / 2 + (right - left - size) * fx), round(top + size / 2 + (bottom - top - size) * fy))
            for fx, fy in ((.25, .75), (.7, .3), (.4, .6), (.8, .55))]


def edge_points():
    """2 px inside each edge and corner: the clamp leaves these pointers outside the plate."""
    left, top, right, bottom = seam.work_area(ANCHOR)
    x, y = ANCHOR
    return [(left + 2, y), (right - 3, y), (x, top + 2), (x, bottom - 3),
            (left + 2, top + 2), (right - 3, top + 2), (left + 2, bottom - 3), (right - 3, bottom - 3)]


def on_plate_points(scale):
    """30 DIP inside the right and the top edge: the clamp moves the plate, but the pointer stays on it."""
    left, top, right, bottom = seam.work_area(ANCHOR)
    inset = round(30 * scale)
    return [('right', (right - inset, ANCHOR[1])), ('top', (ANCHOR[0], top + inset))]


def sector_index(dx, dy, count):
    """src/core.h HitTest outside the center: slot 0 at 12 o'clock, then clockwise."""
    angle = (math.atan2(dy, dx) + math.pi / 2) % (2 * math.pi)
    step = 2 * math.pi / count
    return int((angle + step / 2) // step) % count


def toward(point, origin, distance):
    """point moved distance pixels along the direction from origin to point."""
    dx, dy = point[0] - origin[0], point[1] - origin[1]
    length = math.hypot(dx, dy)
    return round(point[0] + dx / length * distance), round(point[1] + dy / length * distance)


def on_plate_hold(g, side, point, launched):
    """A hold clamped at an edge with the pointer still on the plate. Returns the launch count."""
    reel = g.reel
    offset = len(reel.read_log())
    assert not open_hold(g, point), f'A {side}-edge hold left on the plate needs no center entry.'
    center = reel.center()
    scale = reel.scale()
    distance = math.hypot(point[0] - center[0], point[1] - center[1]) / scale
    assert seam.DEAD_ZONE_DIP < distance <= seam.PLATE_RADIUS_DIP, \
        f'The {side}-edge hold must be clamped off center yet stay on the plate ({distance:.1f} DIP).'
    state = reel.diagnostics()
    assert state['gestureAwaitingCenter'] == '0' and state.selected == -1, state.text
    # The opening point lies over a sector, yet neither it nor a move inside the slop selects.
    reel.pointer(point)
    assert reel.diagnostics().selected == -1, 'The opening position itself must not select.'
    reel.pointer(toward(point, center, -max(1, round(3 * scale))))
    assert reel.diagnostics().selected == -1, f'A move within the {seam.OPENING_SLOP_DIP} DIP opening slop must not select.'
    release(g)
    reel.hidden()
    assert 'Gesture canceled before selection' in reel.read_log()[offset:], 'An unmoved release cancels.'
    assert_no_launch(g, launched)

    offset = len(reel.read_log())
    assert not open_hold(g, point)
    # 10 DIP farther out: past the slop, in the direction the pointer already points.
    moved = toward(point, center, round(10 * scale))
    reel.pointer(moved)
    state = reel.diagnostics()
    expected = sector_index(moved[0] - center[0], moved[1] - center[1], len(state.names))
    assert state.selected == expected and state['gestureAwaitingCenter'] == '0', \
        f'Moving 10 DIP must select sector {expected} at once: {state.text}'
    name = state.names[expected]
    release(g)
    reel.hidden()
    assert 'Center entered' not in reel.read_log()[offset:], 'An on-plate hold never waits for the center.'
    assert_launch(g, launched + 1, name)
    return launched + 1, name


def stable_draws(g, state):
    before = g.reel.diagnostics().surface_draws
    time.sleep(.35)
    after = g.reel.diagnostics().surface_draws
    assert before == after, f'The {state} reel must not redraw unchanged surfaces.'
    EVIDENCE.draws.append({'group': g.name, 'state': state, 'surfaceDrawsBefore': before,
                           'surfaceDrawsAfter': after, 'intervalSeconds': .35})


def close_menu(g, levels):
    for _ in range(levels):
        g.reel.key(seam.VK_ESCAPE)
    g.reel.hidden()


# Groups ------------------------------------------------------------------------------------
def preflight(g):
    reel = g.reel
    g.passed('static self-check: neither driver nor runner names an input-injection API')
    # A misconfigured unpackaged test process exits with 9 before the instance mutex, so it
    # can never fall into the production namespace. --background is a no-op even if it did.
    for label, key, value in (('malformed run id', 'COPILOT_REEL_TEST_INPUT', 'bad id!'),
                              ('relative data directory', 'COPILOT_REEL_DATA_DIR', 'relative\\seam')):
        refused = g.data / ('refused-' + label.split()[0])
        refused.mkdir(parents=True, exist_ok=True)
        env = reel.environment(data=refused)
        env[key] = value
        process = subprocess.Popen([str(EXE), '--background'], env=env, creationflags=seam.CREATE_NO_WINDOW)
        try:
            code = process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
            raise AssertionError(f'A test process with a {label} must exit, not stay resident.')
        assert code == 9, f'A test process with a {label} must exit with 9, not {code}.'
        if key == 'COPILOT_REEL_TEST_INPUT':
            assert 'Test input refused' in (refused / 'run.log').read_text(encoding='utf-8', errors='replace')
    g.passed('a malformed run id or a relative data directory exits with 9 before the instance mutex')

    offset = reel.start()
    assert 'fast-path HWND registered' not in reel.read_log()[offset:], 'A test instance must not register the fast path.'
    assert seam.window_title(reel.hwnd) == reel.title
    state = reel.diagnostics()
    assert state['testInput'] == 'on' and state['testRun'] == reel.run_id, state.text
    assert state.get('realInputIgnored', '').isdigit(), 'realInputIgnored must be a count: ' + state.text
    assert state['activationRequested'] == '0', state.text
    # Topmost from creation: a background start has no foreground rights to raise it later (0.3.6.1).
    assert state['topmost'] == '1', 'The reel window must be topmost before it ever opens: ' + state.text
    g.passed('the test instance owns its own title and namespace, registers no fast path, reports testInput=on, '
             'counts ignored real input, and its window is topmost from creation')

    for description, kwargs in (('version 2', {'version': 2}), ('reserved field', {'reserved': 1}),
                                ('virtual key 0xFF', {'vk': 0xFF})):
        accepted, _ = reel.send_test_packet(seam.KEY, **kwargs)
        assert not accepted, f'A test packet with {description} must be refused.'
    accepted, _ = reel.send_test_packet(seam.ACTION_COUNT)
    assert not accepted, 'An unknown test action must be refused.'
    assert not reel.send_command(seam.DIAGNOSTICS, version=2), 'A CommandPacket of another version must be refused.'
    g.passed('malformed test packets and command packets are refused with FALSE')

    offset = len(reel.read_log())
    open_preview(g, ANCHOR)
    skipped = reel.read_log()[offset:].count(seam.RAW_INPUT_SKIPPED)
    assert skipped == 1, f'Opening a preview must skip Raw Input registration exactly once, not {skipped} times.'
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    assert_quiet(g)
    g.passed("opening a preview logs 'Raw Input skipped: test input seam' and records its activation request "
             'without taking the foreground')
    reel.stop()


def group_a(g):
    """Opening hold: placement, sector release, duplicates, Escape, edges off and on the plate, opening slop, interruption."""
    reel = g.reel
    reel.start()
    offset = len(reel.read_log())
    open_hold(g, ANCHOR)
    state = reel.diagnostics()
    assert state.page == seam.PAGE_MAIN and state.names == APPS + [PLUS], state.names
    assert state.flag('held') and not state.flag('preview') and state['gestureAwaitingCenter'] == '0'
    reel.pointer(ANCHOR)
    assert reel.diagnostics().selected == -1
    release(g)
    reel.hidden()
    assert 'Gesture canceled before selection' in reel.read_log()[offset:], 'An unmoved interior release cancels.'
    assert_no_launch(g, 0)
    g.passed('an interior hold opens on the synthetic pointer with 0 px center displacement; an unmoved release cancels')

    open_hold(g, ANCHOR, 'fast')
    reel.pointer(reel.sector_of('Probe 2'))
    state = reel.diagnostics()
    assert state.selected == state.index('Probe 2')
    rect, starts = reel.rect(), reel.read_log().count('Hold started;')
    assert reel.send_command(seam.HOLD_START, ANCHOR)
    reel.copilot(seam.FAST_HOLD_START)
    reel.sync()
    assert reel.read_log().count('Hold started;') == starts and reel.rect() == rect, 'A duplicate hold-start must not reset the hold.'
    assert reel.diagnostics().selected == state.index('Probe 2')
    assert_no_launch(g, 0)
    release(g, 'fast')
    reel.hidden()
    assert_launch(g, 1, 'Probe 2')
    release(g, 'fast')
    release(g)
    assert_no_launch(g, 1)
    g.passed('the pointer selects a sector, duplicate starts are ignored, and the release launches exactly once')

    open_hold(g, ANCHOR)
    reel.pointer(reel.sector_of('Probe 1'))
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    release(g)
    assert_no_launch(g, 1)
    g.passed('Escape during an opening hold cancels, and its late release launches nothing')

    for point in edge_points():
        assert open_hold(g, point), 'An edge or corner hold that leaves the pointer off the plate waits for center entry.'
        state = reel.diagnostics()
        reel.pointer(reel.sector(0, len(state.names)))
        state = reel.diagnostics()
        assert state.selected == -1 and state['gestureAwaitingCenter'] == '1', 'Crossing a sector before center entry selects nothing.'
        release(g)
        reel.hidden()
    assert_no_launch(g, 1)
    corner = edge_points()[-1]
    open_hold(g, corner)
    target = reel.sector_of('Probe 1')
    reel.pointer(target)
    assert reel.diagnostics().selected == -1
    offset = len(reel.read_log())
    reel.pointer(reel.center_point())
    assert 'Center entered; direction selection armed' in reel.read_log()[offset:]
    reel.pointer(target)
    state = reel.diagnostics()
    assert state.selected == state.index('Probe 1') and state['gestureAwaitingCenter'] == '0'
    release(g)
    reel.hidden()
    assert_launch(g, 2, 'Probe 1')
    g.passed('eight edges and corners 2 px inside the work area clamp minimally and leave the pointer off the plate; '
             'selection arms only after center entry and then launches once')

    # Wall-clock delays are recorded, never asserted: the reel's own markers decide.
    state = reel.diagnostics()
    animated = state['animations'] == '1' and state['highContrast'] == '0'
    reel.pointer(ANCHOR)
    offset = len(reel.read_log())
    began = time.perf_counter()
    reel.copilot(seam.FAST_HOLD_START)
    seam.wait(lambda: 'Hold started;' in reel.read_log()[offset:] and reel.visible(), 'quick opening', interval=.002)
    elapsed = time.perf_counter() - began
    reel.copilot(seam.FAST_HOLD_STOP)
    reel.hidden()
    assert 'Gesture canceled before selection' in reel.read_log()[offset:], 'A release with nothing selected cancels.'
    assert_no_launch(g, 2)
    EVIDENCE.interruptions.append({'group': g.name, 'scenario': 'release during opening',
                                   'releaseAfterMilliseconds': round(elapsed * 1000, 2), 'withinOpening': elapsed < .167})
    g.passed('a release right after the opening hold cancels without launching')

    for attempt in range(1, 4):
        offset = len(reel.read_log())
        reel.copilot(seam.FAST_HOLD_START)
        seam.wait(lambda: 'Hold started;' in reel.read_log()[offset:] and reel.visible(), 'opening before interruption')
        cancel_offset = len(reel.read_log())
        cancelled = time.perf_counter()
        reel.post(seam.WM_CLOSE)
        seam.wait(lambda: 'Canceled' in reel.read_log()[cancel_offset:], 'beginning of the exit animation', interval=.002)
        reopen_offset = len(reel.read_log())
        reopen_after = time.perf_counter() - cancelled
        reel.copilot(seam.FAST_HOLD_START)
        line = seam.wait(lambda: seam.HOLD_LINE.search(reel.read_log()[reopen_offset:]), 'replacement hold').group(0)
        interrupted = line.endswith(seam.INTERRUPTED_EXIT)
        EVIDENCE.interruptions.append({'group': g.name, 'scenario': 're-hold during cancel exit', 'attempt': attempt,
                                       'reopenAfterMilliseconds': round(reopen_after * 1000, 2),
                                       'interruptedExit': interrupted, 'exitAnimated': animated})
        if interrupted or not animated:
            break
        # The runner was too slow and the 83 ms exit had finished: this was an ordinary hold.
        release(g, 'fast')
        reel.hidden()
    if animated:
        assert interrupted, f'A re-hold during the cancel exit must log {seam.INTERRUPTED_EXIT!r}; ' \
                            f'3 attempts did not (last line: {line!r}).'
    else:
        assert not interrupted, 'Without an exit animation there is no exit to interrupt: ' + line
        g.note('skipped: the cancel exit is not animated (animations off or high contrast), so no re-hold can interrupt it')
    time.sleep(.27)
    assert reel.visible(), 'An old closing timer must not hide the replacement hold.'
    state = assert_page(g, seam.PAGE_MAIN, 'Copilot Reel')
    assert state.flag('held') and not state.flag('preview')
    release(g, 'fast')
    reel.hidden()
    assert_no_launch(g, 2)
    if animated:
        g.passed(f"a re-hold during the cancel exit logs 'interrupted=exit' (attempt {attempt}) and survives the old exit")

    open_hold(g, ANCHOR, 'fast')
    reel.select('Probe 0')
    offset = len(reel.read_log())
    # Both posts are handled before the 133 ms commit exit's timer can fire.
    reel.copilot(seam.FAST_HOLD_STOP)
    reel.copilot(seam.FAST_HOLD_START)
    reel.sync()
    assert_launch(g, 3, 'Probe 0')
    match = seam.OPENING.search(reel.read_log()[offset:])
    assert match and match.group(1) == 'Hold started', 'A hold during the commit exit opens a new reel.'
    point = (int(match.group(2)), int(match.group(3)))
    assert point == reel.cursor, 'The new hold opens at the synthetic cursor.'
    line = seam.HOLD_LINE.search(reel.read_log()[offset:]).group(0)
    assert line.endswith(seam.INTERRUPTED_EXIT) == animated, 'A hold queued behind a commit cuts its exit short: ' + line
    verify_placement(g, offset, point, True)
    time.sleep(.27)
    assert reel.visible() and reel.diagnostics().flag('held')
    release(g, 'fast')
    reel.hidden()
    assert_no_launch(g, 3)
    EVIDENCE.interruptions.append({'group': g.name, 'scenario': 're-hold during commit exit',
                                   'interruptedExit': line.endswith(seam.INTERRUPTED_EXIT), 'exitAnimated': animated})
    g.passed('a re-hold during a commit exit survives, launches nothing extra and opens at the synthetic cursor')

    launched = 3
    for side, point in on_plate_points(reel.scale()):
        launched, name = on_plate_hold(g, side, point, launched)
        g.passed(f'a hold 30 DIP inside the {side} edge clamps minimally and stays on the plate without center entry; '
                 f'neither the opening point nor a 3 DIP move selects and the release cancels; '
                 f'10 DIP of movement selects {name!r} at once and its release launches it once')
    assert_quiet(g)
    reel.stop()


def group_b(g):
    """Unified confirmation: Tap, a resumed hold, release, Escape and right click in persistent menus."""
    reel = g.reel
    reel.start()
    reel.copilot(seam.FAST_TAP)
    assert_launch(g, 1, 'Probe 0')
    reel.sync()
    assert not reel.visible(), 'A closed-reel tap launches without showing the reel.'
    g.passed('with the reel closed, a tap launches the preferred agent once without showing the reel')

    open_hold(g, ANCHOR, 'fast')
    rect = reel.rect()
    reel.select(PLUS)
    release(g, 'fast')
    state = assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    assert state.flag('preview') and not state.flag('held') and state.selected == -1, state.text
    assert reel.rect() == rect
    assert_no_launch(g, 1)
    g.passed("hold and release on 'Apps and settings' opens the hub in place as a persistent menu")

    offset = len(reel.read_log())
    reel.copilot(seam.FAST_TAP)
    reel.sync()
    assert 'Confirm ignored: no selection' in reel.read_log()[offset:]
    assert_page(g, seam.PAGE_SETTINGS, 'Settings')
    assert reel.visible()
    assert_no_launch(g, 1)
    g.passed('a tap with nothing selected does nothing and never launches the preferred agent')

    # The release of a resumed hold confirms; the old Release route would have hidden the reel.
    offset = len(reel.read_log())
    reel.copilot(seam.FAST_HOLD_START)
    reel.sync()
    assert 'Hold resumed' in reel.read_log()[offset:]
    reel.pointer(reel.center_point())
    assert reel.diagnostics().selected == -1, 'The plate center selects nothing.'
    reel.copilot(seam.FAST_HOLD_STOP)
    reel.sync()
    assert 'Confirm ignored: no selection' in reel.read_log()[offset:]
    state = assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    assert reel.visible() and state.flag('preview') and not state.flag('held'), state.text
    assert_no_launch(g, 1)
    g.passed('releasing a resumed hold with nothing selected confirms nothing and keeps the hub open')

    offset = len(reel.read_log())
    reel.copilot(seam.FAST_HOLD_START)
    reel.sync()
    log = reel.read_log()[offset:]
    assert 'Hold resumed' in log and 'Hold started;' not in log and reel.rect() == rect, 'A new hold must not move the plate.'
    state = reel.diagnostics()
    assert state.flag('held') and state.flag('preview') and state.page == seam.PAGE_SETTINGS
    reel.select(MANAGE)
    assert reel.diagnostics().selected == HUB.index(MANAGE)
    release(g, 'fast')
    state = assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [ADD])
    assert not state.flag('held')
    release(g, 'fast')
    assert_page(g, seam.PAGE_MANAGE, 'Your apps')
    assert_no_launch(g, 1)
    g.passed('a new hold keeps the plate in place, its release confirms the pointer selection, and a duplicate release is ignored')

    reel.select('Probe 1')
    reel.copilot(seam.FAST_TAP)
    reel.sync()
    state = assert_page(g, seam.PAGE_ENTRY, 'Probe 1')
    assert state.names[0] == 'Probe 1'
    reel.key(seam.VK_ESCAPE)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps')
    reel.select('Probe 2')
    assert reel.send_command(seam.TAP)
    reel.sync()
    assert_page(g, seam.PAGE_ENTRY, 'Probe 2')
    assert_no_launch(g, 1)
    g.passed('in a persistent menu a fast-path or protocol Tap confirms the selected item instead of launching')

    reel.key(seam.VK_ESCAPE)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps')
    reel.right_click()
    assert_page(g, seam.PAGE_SETTINGS, 'Settings')
    reel.key(seam.VK_ESCAPE)
    state = assert_page(g, seam.PAGE_MAIN, 'Copilot Reel', APPS + [PLUS])
    assert state.flag('preview')
    reel.select('Probe 2')
    reel.copilot(seam.FAST_TAP)
    reel.hidden()
    assert_launch(g, 2, 'Probe 2')
    g.passed('Escape and right click go back one level; a Tap in a persistent Main launches the selection, not the preferred agent')

    for back in ('escape', 'right click'):
        open_hold(g, ANCHOR, 'fast')
        reel.select(PLUS)
        release(g, 'fast')
        assert_page(g, seam.PAGE_SETTINGS, 'Settings')
        for _ in range(2):
            if back == 'escape':
                reel.key(seam.VK_ESCAPE)
            else:
                reel.right_click()
        reel.hidden()
        assert_no_launch(g, 2)
    g.passed('Back at Main closes the reel, by Escape and by right click')

    reel.copilot(seam.FAST_TAP)
    assert_launch(g, 3, 'Probe 0')
    reel.sync()
    assert not reel.visible()
    g.passed('after closing, a tap launches the preferred probe exactly once')
    assert_quiet(g)
    reel.stop()


def drag_state(g):
    """(main dragTarget, renderer drag index, renderer trash state) from fresh diagnostics."""
    state = g.reel.diagnostics()
    renderer = dict(re.findall(r'; (drag|trash)=([^;\r\n]*)', state.text))
    return state['dragTarget'], int(renderer['drag']), renderer['trash'], state


def drag_path(reel, start_index, end_index, count, steps=3):
    """Points along the orbit from one slot toward another, past the drag threshold from the first."""
    return [reel.sector(start_index + (end_index - start_index) * (i + 1) / steps, count) for i in range(steps)]


def group_c(g):
    """Manage apps: drag to reorder, drop on the center trash to park an app, put it back from Add, A/D and Delete."""
    reel = g.reel
    reel.start()
    open_preview(g, ANCHOR)
    reel.choose(PLUS)
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    reel.choose(MANAGE)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [ADD])
    reel.choose('Probe 0')
    state = assert_page(g, seam.PAGE_ENTRY, 'Probe 0')
    assert state.names[0] == 'Probe 0' and state['adjust'] == 'none', state.text
    assert all(name in state.names for name in (EDIT, SET_PREFERRED, REMOVE)), state.names
    assert APPS[int(state['preferredIndex'])] == 'Probe 0'
    saved = (g.data / 'apps.ini').read_bytes()
    for vk in (seam.VK_D, seam.VK_RIGHT, seam.VK_A, seam.VK_DELETE):
        reel.key(vk)
    assert_page(g, seam.PAGE_ENTRY, 'Probe 0')
    # Where the earlier chevron was, a click is on the app tile itself: done, back to 'Your apps'.
    reel.click(reel.canvas(*seam.EARLIER_CHEVRON_DIP))
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [ADD])
    assert (g.data / 'apps.ini').read_bytes() == saved, 'An app actions page never reorders.'
    g.passed("the actions page shows the app itself without chevrons; A, D, Right, Delete and the old chevron spot "
             'move nothing, and the tile returns to Your apps')

    # A press and release without movement is still a click: it opens the app's actions.
    start = reel.sector_of('Probe 1')
    reel.press(start)
    step = reel.release_button(start)
    assert step['drag'] == -1 and step['page'] == seam.PAGE_ENTRY, step
    assert_page(g, seam.PAGE_ENTRY, 'Probe 1')
    reel.key(seam.VK_ESCAPE)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [ADD])
    g.passed("a press and release in place on 'Your apps' is a click and opens the app's actions")

    # Drag Probe 0 to Probe 2's slot.
    count = len(APPS) + 1
    offset = len(reel.read_log())
    reel.press(reel.sector(0, count))
    for point in drag_path(reel, 0, 2, count):
        step = reel.pointer(point)
    assert step['drag'] == 0 and step['target'] == 'slot:2', step
    target, lifted, trash, state = drag_state(g)
    assert target == 'slot:2' and lifted == 0 and trash == 'idle', state.text
    assert state['centerLabel'] == '' and state.selected == -1, 'While a tile is dragged the trash owns the center alone.'
    assert (g.data / 'apps.ini').read_bytes() == saved, 'Nothing is saved before the drop.'
    step = reel.release_button(reel.sector(2, count))
    assert step['drag'] == -1 and step['page'] == seam.PAGE_MANAGE, step
    assert reel.apps() == ['Probe 1', 'Probe 2', 'Probe 0'], reel.apps()
    log = reel.read_log()[offset:]
    assert 'Drag started: Probe 0' in log and 'App moved: Probe 0 to position 3' in log, log
    state = assert_page(g, seam.PAGE_MANAGE, 'Your apps', ['Probe 1', 'Probe 2', 'Probe 0', ADD])
    assert 'transition=reflow' in state.text and state.selected == 2, state.text
    assert reel.apps()[int(state['preferredIndex'])] == 'Probe 0', 'Moving an app must not change the preferred agent.'
    assert_no_launch(g, 0)
    g.passed("dragging an app shows the trash, opens the slot under the pointer and saves the order on release; "
             'the page reflows, the moved app stays selected and the preferred agent is unchanged')

    # A drag back to its own slot and a drag canceled with Escape keep the order.
    saved = (g.data / 'apps.ini').read_bytes()
    offset = len(reel.read_log())
    reel.drag(reel.sector(1, count), drag_path(reel, 1, 2, count, 2) + [reel.sector(1, count)], reel.sector(1, count))
    assert 'Drag returned' in reel.read_log()[offset:]
    reel.press(reel.sector(0, count))
    reel.path(drag_path(reel, 0, 1, count))
    reel.key(seam.VK_ESCAPE)
    target, lifted, trash, state = drag_state(g)
    assert target == 'none' and lifted == -1 and trash == 'hidden' and state.page == seam.PAGE_MANAGE, state.text
    step = reel.release_button(reel.sector(1, count))
    assert step['page'] == seam.PAGE_MANAGE, 'The release of a canceled drag is not a click.'
    assert 'Drag canceled' in reel.read_log()[offset:]
    assert (g.data / 'apps.ini').read_bytes() == saved
    g.passed('a drag released on its own slot or canceled with Escape changes nothing, and its release is not a click')

    # Drop Probe 1 on the trash: it leaves the reel with its settings and waits first on Add.
    offset = len(reel.read_log())
    reel.press(reel.sector(0, count))
    reel.path(drag_path(reel, 0, .5, count, 2) + [reel.center_point()])
    target, lifted, trash, state = drag_state(g)
    assert target == 'trash' and trash == 'armed', state.text
    reel.release_button(reel.center_point())
    assert reel.apps() == ['Probe 2', 'Probe 0'], reel.apps()
    assert reel.parked() == ['Probe 1'], reel.parked()
    assert 'App taken out of the reel: Probe 1; parked=1' in reel.read_log()[offset:]
    state = assert_page(g, seam.PAGE_MANAGE, 'Your apps', ['Probe 2', 'Probe 0', ADD])
    assert reel.apps()[int(state['preferredIndex'])] == 'Probe 0'
    assert_no_launch(g, 0)
    g.passed("dropping an app on the armed center trash takes it out of the reel into parked.ini; "
             "'Your apps' stays open and the preferred agent is unchanged")

    reel.choose(ADD)
    state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
    assert state.names[0] == 'Probe 1' and state['addParked'] == '1', state.text
    reel.choose('Probe 1')
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', ['Probe 2', 'Probe 0', 'Probe 1', ADD])
    assert reel.parked() == [], reel.parked()
    ini = (g.data / 'apps.ini').read_text(encoding='utf-16')
    assert 'Name=Probe 1\nKind=shell\nTarget=' in ini and 'Arguments=--radial-probe=1' in ini, ini
    assert not reel.owned_windows(seam.EDITOR_CLASS) and not reel.owned_windows(seam.DIALOG_CLASS)
    assert_no_launch(g, 0)
    g.passed('Add offers the parked app first; one tap puts it back with its own target and arguments, '
             'without the editor or a launch')

    # On 'Your apps' A and D move the selected app and Delete takes it out of the reel.
    reel.select('Probe 1')
    reel.key(seam.VK_A)
    assert reel.apps() == ['Probe 2', 'Probe 1', 'Probe 0'], reel.apps()
    assert reel.diagnostics().selected == 1, 'The selection follows the moved app.'
    reel.key(seam.VK_D)
    reel.key(seam.VK_D)
    assert reel.apps() == ['Probe 2', 'Probe 0', 'Probe 1'], 'D at the last place changes nothing.'
    reel.key(seam.VK_DELETE)
    assert reel.apps() == ['Probe 2', 'Probe 0'] and reel.parked() == ['Probe 1'], (reel.apps(), reel.parked())
    g.passed("on 'Your apps' A and D move the selected app one place and Delete takes it out of the reel")

    # 'Remove from reel' on an app's actions page parks it the same way.
    reel.choose('Probe 2')
    reel.choose(REMOVE)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', ['Probe 0', ADD])
    assert reel.apps() == ['Probe 0'] and reel.parked() == ['Probe 2', 'Probe 1'], (reel.apps(), reel.parked())
    g.passed("'Remove from reel' keeps the app's settings on Add too, most recent first")

    # The last app cannot be dropped on the trash.
    saved = (g.data / 'apps.ini').read_bytes()
    offset = len(reel.read_log())
    reel.press(reel.sector(0, 2))
    reel.path([reel.sector(.25, 2), reel.center_point()])
    target, lifted, trash, state = drag_state(g)
    assert target == 'slot:0' and trash == 'disabled', state.text
    reel.release_button(reel.center_point())
    assert (g.data / 'apps.ini').read_bytes() == saved and reel.parked() == ['Probe 2', 'Probe 1']
    assert 'Drop on the trash ignored: the last app stays' in reel.read_log()[offset:]
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', ['Probe 0', ADD])
    g.passed('with one app left the trash is drawn disabled and a drop on it changes nothing')

    close_menu(g, 3)
    reel.copilot(seam.FAST_TAP)
    assert_launch(g, 1, 'Probe 0')
    g.passed('after reordering and parking, a tap launches the same preferred probe')
    assert_quiet(g)
    reel.stop()


def check_add_page(state, apps, offset):
    catalog = int(state['addCatalog'])
    shown = min(5, max(0, catalog - offset))
    navigation = ([PREVIOUS] if offset > 0 else []) + ([NEXT] if offset + 5 < catalog else [])
    assert state['presetOffset'] == str(offset), state.text
    assert len(state.names) <= 8 and state.names[shown:] == [CUSTOM] + navigation, state.names
    duplicates = set(state.names[:shown]) & set(apps)
    assert not duplicates, f'The Add page lists apps already in the reel: {sorted(duplicates)}'
    return state.names[:shown]


def add_counts(state):
    """(addInstalled, addCatalog): installed, one-tap entries lead the catalog."""
    installed, catalog = int(state['addInstalled']), int(state['addCatalog'])
    assert 0 <= installed <= catalog, state.text
    return installed, catalog


def walk_add_pages(g):
    """Page through Add with Next, then back to the first page; returns each page's catalog names."""
    reel = g.reel
    state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
    offset, pages = 0, [check_add_page(state, reel.apps(), 0)]
    while NEXT in state.names:
        assert len(pages) < 10, 'Add page paging must end.'
        reel.choose(NEXT)
        offset += 5
        state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
        pages.append(check_add_page(state, reel.apps(), offset))
    while PREVIOUS in state.names:
        reel.choose(PREVIOUS)
        offset -= 5
        state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
        check_add_page(state, reel.apps(), offset)
    assert offset == 0
    return pages


def wait_discovery(g, offset):
    """The first discovery completed after offset; returns its catalog size."""
    match = seam.wait(lambda: seam.DISCOVERY.search(g.reel.read_log()[offset:]), 'agent discovery', timeout=30)
    return int(match.group(1))


def group_d(g):
    """Add page: a discovered agent is added in one tap without launching and leaves Add; Custom and paging within 8 entries."""
    reel = g.reel
    reel.start()
    apps_before = (g.data / 'apps.ini').read_bytes()
    open_preview(g, ANCHOR)
    offset = len(reel.read_log())
    reel.choose(PLUS)
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    # Opening the hub runs discovery on a worker; Add is read once it has completed.
    discovered = wait_discovery(g, offset)
    reel.choose(ADD)
    state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
    installed, catalog = add_counts(state)
    assert catalog <= discovered, f'Add cannot offer more than discovery found ({catalog} > {discovered}).'
    pages = walk_add_pages(g)
    sample = {'group': g.name, 'discovered': discovered, 'addInstalled': installed, 'addCatalog': catalog,
              'pages': pages, 'chosen': None}
    EVIDENCE.add.append(sample)
    g.passed(f'Add lists {catalog} catalog entries ({installed} installed) over {len(pages)} page(s), none already '
             'in the reel, with Custom app and Previous/Next within 8 entries')

    if not installed:
        g.note('skipped: no discovered agent on this host, so no one-tap Add was exercised')
        reel.click(reel.center_point())
        assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
        reel.choose(MANAGE)
        reel.choose(ADD)
        assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
        reel.click(reel.center_point())
        assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [ADD])
        assert not reel.owned_windows(seam.EDITOR_CLASS), 'Browsing Add must stay in the reel.'
        assert (g.data / 'apps.ini').read_bytes() == apps_before
        assert_no_launch(g, 0)
        g.passed('the center returns from Add to the hub or to Your apps, whichever opened it')
        close_menu(g, 3)
        assert_quiet(g)
        reel.stop()
        return

    # Installed entries come first, so the first entry of the first page is a one-tap add.
    name = pages[0][0]
    sample['chosen'] = name
    launches = reel.launches()
    offset = len(reel.read_log())
    reel.choose(name)
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [name, ADD])
    assert reel.apps() == APPS + [name], f'apps.ini must gain exactly {name!r}: {reel.apps()}'
    assert not reel.owned_windows(seam.EDITOR_CLASS) and not reel.owned_windows(seam.DIALOG_CLASS), \
        'An installed agent is added without the editor or a picker.'
    assert_no_launch(g, 0)
    assert reel.launches() == launches
    g.passed(f"choosing the installed {name!r} saves it to apps.ini and opens 'Your apps' without launching anything")

    # 'Your apps' runs discovery again; Add is read once it has completed.
    wait_discovery(g, offset)
    reel.choose(ADD)
    state = assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
    after = add_counts(state)
    sample['after'] = {'addInstalled': after[0], 'addCatalog': after[1]}
    assert after == (installed - 1, catalog - 1), f'Adding {name!r} must remove exactly it from Add: ' \
                                                  f'{(installed, catalog)} -> {after}'
    pages = walk_add_pages(g)
    assert all(name not in page for page in pages), f'{name!r} is still offered on Add: {pages}'
    g.passed(f'Add no longer offers {name!r}: addCatalog and addInstalled each dropped by one')

    reel.click(reel.center_point())
    assert_page(g, seam.PAGE_MANAGE, 'Your apps', APPS + [name, ADD])
    reel.click(reel.center_point())
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    reel.choose(ADD)
    assert_page(g, seam.PAGE_PRESETS, 'Add an agent')
    reel.click(reel.center_point())
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    assert reel.apps() == APPS + [name]
    assert_no_launch(g, 0)
    g.passed('the center returns from Add to Your apps or to the hub, whichever opened it')
    close_menu(g, 2)
    assert_quiet(g)
    reel.stop()


def group_e(g):
    """A Shell minimize never leaves the reel iconic, and an iconic reel is restored before placement."""
    reel = g.reel
    reel.start()
    open_preview(g, ANCHOR)
    size = reel.rect()[2] - reel.rect()[0]
    offset = len(reel.read_log())
    reel.post(seam.WM_SYSCOMMAND, seam.SC_MINIMIZE)
    reel.sync()
    assert not reel.iconic() and reel.visible(), 'A minimize command must not minimize an open preview.'
    assert reel.diagnostics().flag('gestureActive')
    assert 'Shell window command ignored' in reel.read_log()[offset:]
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    first, second, third, fourth = interior_points(size)
    open_hold(g, first, 'fast')
    release(g, 'fast')
    reel.hidden()
    reel.post(seam.WM_SYSCOMMAND, seam.SC_MINIMIZE)
    reel.sync()
    assert not reel.iconic(), 'A minimize command must not minimize the hidden reel.'
    g.passed('SC_MINIMIZE on an open or hidden reel is ignored')

    # Minimized by other means while hidden: no activation, no pointer movement.
    reel.minimize_quietly()
    assert reel.iconic()
    assert_quiet(g)
    offset = len(reel.read_log())
    open_hold(g, fourth, 'fast')
    assert 'Minimized reel window restored before placement' in reel.read_log()[offset:]
    assert not reel.iconic(), 'The hold must open a restored, not an iconic, reel.'
    release(g, 'fast')
    reel.hidden()
    assert not reel.iconic()
    g.passed('an iconic hidden reel is restored before placement, and the next hold is placed exactly around its point')
    open_preview(g, second)
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    open_hold(g, third, 'fast')
    release(g, 'fast')
    reel.hidden()
    assert_no_launch(g, 0)
    g.passed('the next openings are placed around their own points')
    assert_quiet(g)
    reel.stop()


def group_fast_path(g):
    """The registered fast-path Tap/Down/Up message contract."""
    reel = g.reel
    reel.start()
    open_hold(g, ANCHOR, 'fast')
    reel.select('Probe 0')
    release(g, 'fast')
    reel.hidden()
    assert_launch(g, 1, 'Probe 0')
    release(g, 'fast')
    assert_no_launch(g, 1)
    reel.copilot(seam.FAST_TAP)
    assert_launch(g, 2, 'Probe 0')
    g.passed('the fast-path Tap/Down/Up message contract dispatches exactly once')
    assert_quiet(g)
    reel.stop()


def group_interior(g):
    """A distinct interior point stays the exact plate center without a guard."""
    reel = g.reel
    reel.start()
    open_hold(g, ANCHOR)
    size = reel.rect()[2] - reel.rect()[0]
    release(g)
    reel.hidden()
    point = interior_points(size)[0]
    assert not open_hold(g, point), 'An interior hold needs no center entry.'
    assert reel.center() == (float(point[0]), float(point[1])), 'An off-center desktop point remains the exact plate center.'
    release(g)
    reel.hidden()
    assert_no_launch(g, 0)
    g.passed('a distinct interior desktop point is the exact plate center with no guard')
    assert_quiet(g)
    reel.stop()


def hub_hover(g, names, source, keyboard=False):
    reel = g.reel
    for index, name in enumerate(names):
        reel.pointer(reel.sector(index, len(names)))
        assert center_label(g, name, f'{source} pointer sector {index}').selected == index
    for chevron in (seam.EARLIER_CHEVRON_DIP, seam.LATER_CHEVRON_DIP):
        reel.pointer(reel.canvas(*chevron))
        assert center_label(g, names[0], f'{source} chevron {chevron[0]}').selected == 0
    reel.pointer(reel.center_point())
    center_label(g, '', f'{source} idle hub')
    if keyboard:
        for index, name in enumerate(names):
            reel.key(seam.VK_TAB)
            assert center_label(g, name, f'{source} Tab {index}').selected == index
        reel.key(seam.VK_TAB)
        center_label(g, names[0], f'{source} Tab wrap')
        reel.pointer(nudge(reel))
        center_label(g, '', f'{source} idle after keyboard')


def group_hub(g):
    """Hub hover names in both languages, Tab, and the language adjust by keys, chevrons and icon."""
    reel = g.reel
    reel.start()
    apps_before = (g.data / 'apps.ini').read_bytes()
    open_preview(g, ANCHOR)
    assert_page(g, seam.PAGE_MAIN, 'Copilot Reel', APPS + [PLUS])
    reel.pointer(reel.sector_of('Probe 0'))
    center_label(g, 'Probe 0', 'main app hover')
    reel.pointer(reel.sector_of(PLUS))
    center_label(g, PLUS, 'main combined entry hover')
    reel.pointer(reel.center_point())
    center_label(g, '', 'idle main')
    reel.choose(PLUS)
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    assert not reel.owned_windows(seam.EDITOR_CLASS), 'The combined hub stays in the reel.'
    hub_hover(g, HUB, 'en-US', keyboard=True)
    g.passed('EN hub hover names, adjust chevrons, idle center and Tab selection with wrap are correct')

    reel.key(seam.VK_D)
    wait_language(g, 'zh-CN')
    assert_page(g, seam.PAGE_SETTINGS, '设置', HUB_ZH)
    hub_hover(g, HUB_ZH, 'zh-CN')
    reel.key(seam.VK_A)
    wait_language(g, 'en-US')
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    g.passed('ZH hub hover names are correct; A and D switch the language on the hub')

    reel.click(reel.canvas(*seam.LATER_CHEVRON_DIP))
    wait_language(g, 'zh-CN')
    reel.click(reel.canvas(*seam.EARLIER_CHEVRON_DIP))
    wait_language(g, 'en-US')
    reel.choose(LANGUAGE)
    wait_language(g, 'zh-CN')
    reel.choose(HUB_ZH[0])
    wait_language(g, 'en-US')
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    assert (g.data / 'apps.ini').read_bytes() == apps_before, 'Switching language never changes the apps.'
    assert_no_launch(g, 0)
    g.passed('both chevrons and the language icon switch and persist the language; the app profile is shared')
    close_menu(g, 2)
    assert_quiet(g)
    reel.stop()


def set_preferred(g, name):
    """The preferred agent is set from the app's own actions page; the hub has no Preferred entry."""
    reel = g.reel
    open_preview(g, ANCHOR)
    reel.choose(PLUS)
    assert_page(g, seam.PAGE_SETTINGS, 'Settings', HUB)
    reel.choose(MANAGE)
    reel.choose(name)
    assert_page(g, seam.PAGE_ENTRY, name)
    reel.choose(SET_PREFERRED)
    state = assert_page(g, seam.PAGE_MANAGE, 'Your apps')
    assert reel.apps()[int(state['preferredIndex'])] == name
    reel.click(reel.center_point())
    assert_page(g, seam.PAGE_SETTINGS, 'Settings')
    reel.click(reel.center_point())
    assert_page(g, seam.PAGE_MAIN, 'Copilot Reel')
    close_menu(g, 1)


def group_preferred(g):
    """Setting the preferred agent from an app's actions changes what a tap opens."""
    reel = g.reel
    reel.start()
    set_preferred(g, 'Probe 1')
    assert_no_launch(g, 0)
    reel.copilot(seam.FAST_TAP)
    assert_launch(g, 1, 'Probe 1')
    g.passed("'Set as preferred' on an app's actions page changes short-tap behavior and returns to 'Your apps'; "
             'the center and Escape navigate back')
    assert_quiet(g)
    reel.stop()


def group_draws(g):
    """A stationary or hidden reel performs no repeated surface drawing."""
    reel = g.reel
    reel.start()
    open_preview(g, ANCHOR)
    time.sleep(.4)
    stable_draws(g, 'visible and stationary')
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    time.sleep(.2)
    stable_draws(g, 'hidden')
    g.passed('stationary and hidden reels perform no repeated surface drawing')
    assert_quiet(g)
    reel.stop()
    g.passed('the test instance shuts down cleanly on Quit')


def group_cold(g):
    """Cold tap, cold hold at COPILOT_REEL_TEST_POINTER, protocol Tap through a second process, orphan release, bare start."""
    reel = g.reel
    reel.start()
    set_preferred(g, 'Probe 1')
    reel.stop()

    reel.start('--tap')
    assert_launch(g, 1, 'Probe 1')
    reel.sync()
    assert not reel.visible()
    assert reel.apps()[int(reel.diagnostics()['preferredIndex'])] == 'Probe 1'
    reel.stop()
    g.passed('a cold short tap launches the persisted preferred agent without showing the reel')

    offset = reel.start('--hold-start', pointer=ANCHOR)
    verify_placement(g, offset, ANCHOR, True)
    time.sleep(.1)
    state = reel.diagnostics()
    assert state.flag('gestureActive') and state.flag('held'), \
        'A cold test hold must stay open: the seam must not poll the real Copilot key state (WatchCopilotKey).'
    reel.select('Probe 0')
    release(g)
    reel.hidden()
    assert_launch(g, 2, 'Probe 0')
    assert reel.client('--copilot-uri', 'copilot-reel://?state=Tap') == 0, 'The protocol client must hand off to the test instance.'
    assert_launch(g, 3, 'Probe 1')
    reel.stop()
    g.passed('a cold hold opens at COPILOT_REEL_TEST_POINTER; a protocol Tap from a second process uses the preferred agent')

    reel.start('--hold-stop')
    reel.sync()
    assert not reel.visible()
    release(g)
    assert_no_launch(g, 3)
    assert_quiet(g)
    reel.stop()
    g.passed('orphan cold and duplicate release events neither open the reel nor launch')

    # No argument at all. Unpackaged, there is no activation to tell a key tap from a deliberate
    # start, and the seam reads the Copilot key as not held, so the start opens the preview.
    offset = reel.start(None, pointer=ANCHOR)
    match = seam.BARE_START.search(reel.read_log()[offset:])
    assert match, 'A bare start logs its activation and command: ' + reel.read_log()[offset:]
    verify_placement(g, offset, ANCHOR, False)
    state = reel.diagnostics()
    assert state.page == seam.PAGE_MAIN and state.flag('preview') and not state.flag('held'), state.text
    assert_no_launch(g, 3)
    reel.key(seam.VK_ESCAPE)
    reel.hidden()
    assert_no_launch(g, 3)
    assert_quiet(g)
    reel.stop()
    g.passed(f"a bare start of a fresh test instance logs '{match.group(1)}' and opens the preview without launching")


GROUPS = {
    'a': group_a, 'b': group_b, 'c': group_c, 'd': group_d, 'e': group_e,
    'fast-path': group_fast_path, 'interior': group_interior, 'hub': group_hub,
    'preferred': group_preferred, 'draws': group_draws, 'cold': group_cold,
}


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', type=Path, default=ROOT / 'build/Release/CopilotReel.exe')
    parser.add_argument('--probe', type=Path, default=ROOT / 'build/Release/launch_probe.exe')
    parser.add_argument('--output', type=Path, help='A new, empty directory under artifacts/qa.')
    parser.add_argument('--groups', help='Comma-separated groups to run after the preflight: ' + ', '.join(GROUPS))
    parser.add_argument('--visual', help='Optional COPILOT_REEL_VISUAL_OVERRIDE for every test instance.')
    parser.add_argument('--list', action='store_true', help='List the groups and exit.')
    return parser.parse_args()


def run_group(name, function, index):
    g = Group(name, index)
    entry = {'description': (function.__doc__ or '').strip(), 'runId': g.run_id, 'dataDirectory': str(g.data)}
    began = time.perf_counter()
    try:
        function(g)
        entry['status'] = 'passed'
    except Exception:
        entry['status'] = 'failed'
        entry['error'] = traceback.format_exc()
        try:
            if g.reel.process is not None and g.reel.process.poll() is None:
                state = g.reel.diagnostics()
                entry['finalDiagnostics'] = state.values
        except Exception:
            pass
    finally:
        g.reel.shutdown()
    log = g.reel.read_log()
    if entry['status'] == 'passed' and ('Window error:' in log or 'Unexpected window error' in log):
        entry['status'], entry['error'] = 'failed', 'run.log records a window error'
    entry['checks'] = g.checks
    if g.notes:
        entry['notes'] = g.notes
    # The group's last diagnostics: real input that reached the test window and was dropped.
    diagnostics = g.data / 'diagnostics.txt'
    if diagnostics.exists():
        values = seam.Diagnostics(diagnostics.read_text(encoding='utf-8', errors='replace')).values
        entry['realInputIgnored'] = values.get('realInputIgnored')
        entry['activationRequested'] = values.get('activationRequested')
    entry['seconds'] = round(time.perf_counter() - began, 2)
    return entry


def main():
    global ARGS, EXE, PROBE, DATA, SESSION, ANCHOR, EVIDENCE
    ARGS = parse_arguments()
    if ARGS.list:
        for name, function in GROUPS.items():
            print(f'{name:10} {(function.__doc__ or "").strip()}')
        return 0
    selected = list(GROUPS) if not ARGS.groups else [name.strip() for name in ARGS.groups.split(',') if name.strip()]
    unknown = [name for name in selected if name not in GROUPS]
    if unknown:
        raise SystemExit('Unknown groups: ' + ', '.join(unknown) + '. Known: ' + ', '.join(GROUPS))
    EXE, PROBE = ARGS.exe.resolve(), ARGS.probe.resolve()
    assert EXE.is_file() and PROBE.is_file(), 'Build the app and the launch probe first (tools/build.ps1).'
    seam.assert_seam_binary(EXE)
    qa_root = (ROOT / 'artifacts/qa').resolve()
    SESSION = uuid.uuid4().hex[:12]
    DATA = (ARGS.output or qa_root / ('seam-' + time.strftime('%Y%m%d-%H%M%S') + '-' + SESSION[:6])).resolve()
    assert DATA.is_relative_to(qa_root) and DATA != qa_root, 'Evidence must be a child of artifacts/qa.'
    assert not DATA.exists(), 'Use a fresh evidence directory; earlier evidence is preserved.'
    DATA.mkdir(parents=True)
    left, top, right, bottom = seam.primary_work_area()
    ANCHOR = ((left + right) // 2, (top + bottom) // 2)
    EVIDENCE = Evidence()
    installed_before = seam.installed_window()
    foreground_before = seam.foreground()

    results = {}
    for index, name in enumerate(['preflight'] + selected):
        function = preflight if name == 'preflight' else GROUPS[name]
        print(f'[{name}] ...', flush=True)
        results[name] = run_group(name, function, index)
        print(f'[{name}] {results[name]["status"]} ({results[name]["seconds"]} s)', flush=True)
        for note in results[name].get('notes', []):
            print(f'[{name}] note: {note}', flush=True)
        if name == 'preflight' and results[name]['status'] != 'passed':
            print('Preflight failed; no further group runs against this binary.', flush=True)
            break

    installed_after = seam.installed_window()
    failed = [name for name, entry in results.items() if entry['status'] != 'passed']
    report = {
        'runner': 'tests/run-reel-seam.py',
        'passed': not failed and len(results) == 1 + len(selected),
        'failedGroups': failed,
        'groups': results,
        'inputInjection': 'none',
        'staticSelfCheck': {'sources': CHECKED_SOURCES, 'forbidden': list(FORBIDDEN), 'found': []},
        'installedInstanceResident': bool(installed_before),
        'installedInstanceStillResident': bool(installed_after),
        'installedInstanceUnchanged': installed_before == installed_after,
        'seamSession': SESSION,
        'binary': str(EXE), 'binarySha256': seam.sha256(EXE), 'probe': str(PROBE),
        'anchor': list(ANCHOR), 'primaryWorkArea': [left, top, right, bottom],
        'foregroundAtStart': {'hwnd': foreground_before[0], 'pid': foreground_before[1]},
        'placements': EVIDENCE.placements, 'interruptionSamples': EVIDENCE.interruptions,
        'hoverSamples': EVIDENCE.hover, 'drawSamples': EVIDENCE.draws, 'foregroundSamples': EVIDENCE.foreground,
        'addSamples': EVIDENCE.add,
        'evidenceDirectory': str(DATA),
        'note': 'Synthetic input skips system routing (WM_NCHITTEST/WM_MOUSEACTIVATE dispatch, Raw Input, '
                'foreground activation, IME). Those, the physical Copilot key and editor/picker groups need '
                'the separate real-input checks in docs/testing-plan.md.',
    }
    (DATA / 'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'failedGroups': failed, 'evidence': str(DATA / 'results.json')}))
    for name in failed:
        print(f'--- {name} ---\n{results[name].get("error", "")}', file=sys.stderr)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
