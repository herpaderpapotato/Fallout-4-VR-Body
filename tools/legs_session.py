"""20 Nov FRIK agent: C42 VRIK-style legs - targeted functional numbers (no visual judging; the user tests in the headset).

No-XR launch, MO2 profile TestProfileVR, latest save. Phases (each reads the `FRIK-flat C42 legs:` 2 s windows in FRIK.log):
  stand 4 s  -> planted: solved foot move ~0, no steps, foot - target ~0
  walk 4 s   -> moving frames, moveMode direction bits set, foot - target (IK reach)
  stand 3 s  -> re-planted after the stop
  turn 3 s   -> stick turn on the spot: steps happen, standing animation foot range (turn-in-place shuffle?)
  sneak walk -> moving while sneaking
  live switch: cfgui set FRIK bAnimatedLegs 0 -> FRIK logs OFF; discard -> on again
Run: python -I frik-flat/tools/legs_session.py [--no-deploy]
"""
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "plugin", "tools"))
import fo4test as t  # noqa: E402

OFF_FLAG = os.path.join(os.path.dirname(t.MOD_INI), "fo4flatvr_frik_bodygraph_off.test")
TEST_SAVE = "c43_legs_test"
SAVES = os.path.expandvars(r"%LOCALAPPDATA%\ModOrganizer\Fallout 4\profiles\TestProfileVR\saves")
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
    lines = [l.split("] ", 1)[-1] for l in new.splitlines() if "C42" in l or "C43" in l or "C39 legs" in l]
    t.say(f"=== {name} ({action or 'idle'}, {seconds} s): {len(lines)} lines")
    for l in lines:
        print("    " + l[:900])
    return lines


def main():
    if "--no-deploy" not in sys.argv:
        t.deploy_frik(tag="c43-bodygraph")
    ini_before = open(t.MOD_INI, "rb").read()
    save = t.latest_save()
    try:
        with t.Game(no_xr=True, frik=True) as g:
            g.cmd(f"console load {save}")
            time.sleep(2.0)
            g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
            time.sleep(4.0)
            fl = frik_log()
            for l in fl.splitlines():
                if "C42:" in l or "C36: calibrated" in l or ("C43" in l and "body graph:" in l and "steps" not in l):
                    print("    " + l[:900])
            # the user's ini may still hold bAnimatedLegs=0 (rolled back 20 Nov): preview on (live, not saved)
            phase(g, "preview bAnimatedLegs 1", "cfgui set FRIK bAnimatedLegs 1", 2.0)
            phase(g, "stand", None, 4.5)
            phase(g, "walk forward", "pad ly=1 ms=4000", 4.5)
            phase(g, "stand after walk", None, 4.5)
            phase(g, "turn on the spot (stick)", "pad rx=0.5 ms=4000", 4.5)
            phase(g, "stand after turn", None, 2.5)
            phase(g, "strafe right", "pad lx=1 ms=3000", 3.5)
            phase(g, "strafe left", "pad lx=-1 ms=3000", 3.5)
            phase(g, "walk back", "pad ly=-1 ms=3000", 3.5)
            g.cmd('console player.cf "Actor.StartSneaking"')
            phase(g, "sneak stand", None, 2.5)
            phase(g, "sneak walk", "pad ly=1 ms=3000", 3.5)
            g.cmd('console player.cf "Actor.StopSneaking"')
            phase(g, "stand (sneak off)", None, 2.5)
            g.cmd('console player.cf "Actor.DrawWeapon"')
            phase(g, "weapon drawn stand", None, 3.0)
            phase(g, "drawn walk forward", "pad ly=1 ms=3000", 3.5)
            phase(g, "drawn strafe left", "pad lx=-1 ms=3000", 3.5)
            phase(g, "drawn walk back", "pad ly=-1 ms=3000", 3.5)
            # live off switch: test flag next to fo4flatvr.ini (the bBodyGraph panel row needs a fo4flatvr build)
            with open(OFF_FLAG, "w") as fh:
                fh.write("legs_session.py body graph off test")
            phase(g, "body graph off (flag)", None, 1.5)
            phase(g, "body graph off walk", "pad ly=1 ms=3000", 3.5)
            os.replace(OFF_FLAG, OFF_FLAG + time.strftime(".old-%Y%m%d-%H%M%S"))
            phase(g, "body graph back on", None, 1.5)
            phase(g, "body graph on walk", "pad ly=1 ms=3000", 3.5)
            phase(g, "live switch off", "cfgui set FRIK bAnimatedLegs 0", 3.0)
            phase(g, "live switch back (discard previews)", "cfgui discard", 3.0)
            g.cmd("status")
    finally:
        if os.path.exists(OFF_FLAG):
            os.replace(OFF_FLAG, OFF_FLAG + time.strftime(".old-%Y%m%d-%H%M%S"))
        # the test save must not become the profile's "latest save" for other runs: renamed away (never deleted)
        for ext in (".fos", ".f4se"):
            f = os.path.join(SAVES, TEST_SAVE + ext)
            if os.path.exists(f):
                os.replace(f, f + time.strftime(".old-%Y%m%d-%H%M%S"))
                t.say(f"test save renamed: {f}")
        t.say(f"fo4flatvr.ini untouched: {open(t.MOD_INI, 'rb').read() == ini_before}")


if __name__ == "__main__":
    main()
