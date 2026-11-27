"""20 Nov FRIK agent: C43b body graph walk speed - targeted numbers (no visual judging; the user tests in the headset).

User report: "When walking, the leg walking animation is extremely slow. When running it is fine."
Reads the `FRIK-flat C43b body graph speed:` 2 s windows in FRIK.log: graph 0's Speed (raw desired speed -> graph 0's own
speed-contour value), the playback adjustments applied, and the gait (foot cycle period, ground speed, cycle length).
A/B: [FRIK] bBodyGraphSpeedContour 1 (fix) vs 0 (C43 as first shipped: graph 1's channel value), walk (stick 0.4) and run (1.0).
No-XR launch, MO2 profile TestProfileVR, latest save; nothing saved; previews discarded at the end.
Run: python -I frik-flat/tools/walkspeed_session.py [--no-deploy]
"""
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "plugin", "tools"))
import fo4test as t  # noqa: E402

FRIK_LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\FRIK.log")


def frik_log():
    try:
        return open(FRIK_LOG, "rb").read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def phase(g, name, action, seconds):
    n0 = len(frik_log())
    if action:
        g.cmd(action)
    time.sleep(seconds)
    new = frik_log()[n0:]
    lines = [l.split("] ", 1)[-1] for l in new.splitlines() if "C43b" in l]
    t.say(f"=== {name} ({action or 'idle'}, {seconds} s): {len(lines)} lines")
    for l in lines:
        print("    " + l[:1200])
    return lines


def walk_run(g, tag):
    phase(g, f"{tag}: stand", None, 2.5)
    phase(g, f"{tag}: walk (stick 0.4)", "pad ly=0.4 ms=6500", 7.0)
    phase(g, f"{tag}: stand", None, 2.5)
    phase(g, f"{tag}: walk (stick 0.6)", "pad ly=0.6 ms=6500", 7.0)
    phase(g, f"{tag}: stand", None, 2.5)
    phase(g, f"{tag}: run (stick 1.0)", "pad ly=1 ms=6500", 7.0)


def main():
    if "--no-deploy" not in sys.argv:
        t.deploy_frik(tag="c43b-walkspeed")
    ini_before = open(t.MOD_INI, "rb").read()
    save = t.latest_save()
    with t.Game(no_xr=True, frik=True) as g:
        g.cmd(f"console load {save}")
        time.sleep(2.0)
        g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
        time.sleep(4.0)
        for l in frik_log().splitlines():
            if "C43b" in l and "installed" in l:
                print("    " + l[:900])
        phase(g, "preview bAnimatedLegs 1 + bBodyGraph 1", "cfgui set FRIK bAnimatedLegs 1", 1.0)
        phase(g, "preview", "cfgui set FRIK bBodyGraph 1", 1.0)
        phase(g, "preview speed fix on", "cfgui set FRIK bBodyGraphSpeedContour 1", 1.5)
        walk_run(g, "fix ON")
        phase(g, "speed fix off (live)", "cfgui set FRIK bBodyGraphSpeedContour 0", 2.5)
        walk_run(g, "fix OFF")
        phase(g, "discard previews", "cfgui discard", 2.5)
        g.cmd("status")
    t.say(f"fo4flatvr.ini untouched: {open(t.MOD_INI, 'rb').read() == ini_before}")


if __name__ == "__main__":
    main()
