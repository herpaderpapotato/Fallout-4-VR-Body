"""FRIK-flat in-game session: deploy FRIK.dll (+ the current fo4flatvr.dll), launch through plugin/tools/fo4test.Game,
go in world, collect FRIK.log and fo4flatvr.log's [FRIK] lines, quit, and switch FRIK.dll off again
(renamed to FRIK.dll.frikflat-off, which F4SE does not load) so the user's sessions are unaffected.

Usage: python -I frik-flat/tools/frik_session.py [--xr steamvr|none] [--seconds 20] [--keep] [--no-weapon]
  default: SteamVR + vremu (user rule 14 Nov; --xr none only with a stated reason), `load vrtest_sanctuary`
  (made here from `coc SanctuaryExt` once the quest notifications cleared, if it does not exist; never QASmoke),
  10mm equipped, vremu hands posed in front, `frik perf` FRIK frame on/off/on, two small captures (left eye, forward and looking
  down; the BMPs go to D:/Fallout4ToVR-overflow/frik-captures). Never starts, closes or restarts SteamVR (refuses without vremu).
  Bridge v3: the run is in full FRIK-flat mode via the flag file fo4flatvr_frik_body.test next to fo4flatvr.ini (in the
  fo4flatvr MO2 mod folder = Data/F4SE/Plugins in the VFS; created before the launch, renamed to .off afterwards; the user's
  ini is never touched). FRIK.dll stays deployed (shipped for smooth movement).
  MO2 (since 15 Nov 21:16): deploys go into the MO2 mod folders (fo4flatvr -> mods/fo4flatvr, FRIK.dll + FRIK assets ->
  mods/FRIK-flat; previous files to work/deploy-backup/), never the base Data folder; the launch goes through MO2 (fo4test)
  and refuses unless both mods are enabled in profile TestProfile. The env override FO4FLATVR_FRIK_BODY does not survive MO2's
  hand-off when its window is open; the flag file does.
  --keep  (no effect since 15 Nov)
Refuses while Fallout4 runs, work/fo4test.hold exists or an xmake build runs.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "plugin", "tools"))
import fo4test  # noqa: E402

PLUG = fo4test.PLUG            # fo4flatvr MO2 mod: F4SE/Plugins (fo4flatvr.dll/.ini; the test flag files go next to the ini)
FRIK_PLUG = fo4test.FRIK_PLUG  # FRIK-flat MO2 mod: F4SE/Plugins (FRIK.dll/.pdb)
FRIK_BUILD = fo4test.FRIK_BUILD
PLUGIN_BUILD = fo4test.BUILD
LOGDIR = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE")
OUT = os.path.join(ROOT, "work", "frik-flat")
OFF = "FRIK.dll.frikflat-off"


def say(m):
    fo4test.say(m)


def busy():
    if fo4test.game_pid():
        return "Fallout4 running"
    if os.path.exists(fo4test.HOLD):
        return "hold file"
    if "xmake" in subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower():
        return "xmake running"
    return None


def deploy():
    """Current fo4flatvr + FRIK builds into their MO2 mod folders (fo4test.deploy_*: backup to work/deploy-backup, refuses
    while a game / xmake / another launch runs). FRIK's own assets go to the FRIK-flat mod, FRIK-named folders only, new
    files only (never FRIK's Meshes/Pipboy, Meshes/Actors or its ESP)."""
    fo4test.deploy_plugin(PLUGIN_BUILD)
    fo4test.deploy_frik(FRIK_BUILD)
    src_root = os.path.join(ROOT, "frik-flat", "frik-data", "mod")
    added = 0
    for sub in ("Meshes/FRIK", "Textures/FRIK", "Materials/FRIK"):
        for dirpath, _, files in os.walk(os.path.join(src_root, sub)):
            for fn in files:
                rel = os.path.relpath(os.path.join(dirpath, fn), src_root)
                dst = os.path.join(fo4test.FRIK_MOD_DIR, rel)
                if not os.path.exists(dst):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(os.path.join(dirpath, fn), dst)
                    added += 1
    say(f"deployed fo4flatvr.dll + FRIK.dll to the MO2 mods" + (f" (+{added} new FRIK asset files)" if added else ""))


BODY_FLAG = os.path.join(PLUG, "fo4flatvr_frik_body.test")


def undeploy():
    """Since 15 Nov FRIK.dll is shipped (bridge v3, smooth-movement-only unless bBody=1): only the body test flag goes."""
    if os.path.exists(BODY_FLAG):
        os.replace(BODY_FLAG, BODY_FLAG + ".off")
        say("body test flag off (FRIK.dll stays deployed: smooth-movement-only mode)")


STEAMVR_JSON = r"E:\SteamLibrary\steamapps\common\SteamVR\steamxr_win64.json"  # = plugin/tools/suite.py
SAVE = "vrtest_sanctuary"
SAVES = fo4test.saves_dir()  # MO2 profile-local saves (TestProfile: LocalSaves=true); fo4test copies the vrtest_* saves there


def vremu(q):
    try:
        return urllib.request.urlopen("http://127.0.0.1:8766" + q, timeout=3).read().decode("utf-8", "replace")
    except OSError as e:
        return f"vremu error {e}"


def proc_running(name):
    return name.lower() in subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}"], capture_output=True, text=True).stdout.lower()


# SteamVR (vremu emulator mode) is shared with other agents: this script may start it but NEVER closes, kills or
# restarts it (coordinator rule 14 Nov); the coordinator closes it at the end of the day.


def save_exists():
    return any(f.lower().startswith(SAVE.lower()) and f.lower().endswith(".fos") for f in os.listdir(SAVES))


def to_world(g):
    if save_exists():
        g.cmd(f"console load {SAVE}")
        time.sleep(2.0)
        g._wait(f"in-world ({SAVE})", 90, lambda: g.ready().get("ingame") == "1")
        time.sleep(2.0)
    else:
        g.coc("SanctuaryExt")
        say("no vrtest_sanctuary save yet: waiting 20 s for the quest notifications, then saving it")
        time.sleep(20.0)
        say(f"save: {g.cmd(f'console save {SAVE}')}")
        time.sleep(3.0)


OVERFLOW = r"D:\Fallout4ToVR-overflow\frik-captures"


def small_png(bmp, png, left_eye=True, scale=0.25):
    """Left eye (left half of the double-wide capture), downscaled, as PNG; the big BMP is moved to the overflow drive."""
    ps = ("Add-Type -AssemblyName System.Drawing; $im=[System.Drawing.Image]::FromFile('%s'); $w=$im.Width; if(%d){$w=[int]($im.Width/2)}; "
          "$ow=[int]($w*%f); $oh=[int]($im.Height*%f); $bm=New-Object System.Drawing.Bitmap $ow,$oh; $g=[System.Drawing.Graphics]::FromImage($bm); "
          "$g.DrawImage($im,(New-Object System.Drawing.Rectangle 0,0,$ow,$oh),(New-Object System.Drawing.Rectangle 0,0,$w,$im.Height),"
          "[System.Drawing.GraphicsUnit]::Pixel); $bm.Save('%s',[System.Drawing.Imaging.ImageFormat]::Png); $im.Dispose(); $bm.Dispose()"
          % (bmp.replace("/", "\\"), 1 if left_eye else 0, scale, scale, png.replace("/", "\\")))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    os.makedirs(OVERFLOW, exist_ok=True)
    try:
        shutil.move(bmp, os.path.join(OVERFLOW, os.path.basename(bmp)))
    except OSError:
        pass
    return os.path.exists(png)


def capture(g, stem, both=False):
    bmp = os.path.join(OUT, stem + ".bmp").replace("\\", "/")
    g.cmd("capture " + bmp, quiet=True)
    t0 = time.time()
    while time.time() - t0 < 10 and not (os.path.exists(bmp) and os.path.getsize(bmp) > 1000):
        time.sleep(0.2)
    time.sleep(0.5)
    png = os.path.join(OUT, stem + ".png")
    return png if os.path.exists(bmp) and (small_png(bmp, png, left_eye=False, scale=0.15) if both else small_png(bmp, png)) else None


def main():
    # bridge v3: full FRIK-flat (body) only with [FRIK] bBody=1; this test override keeps the user's ini untouched
    os.environ["FO4FLATVR_FRIK_BODY"] = "1"
    ap = argparse.ArgumentParser()
    ap.add_argument("--xr", default="steamvr", choices=["none", "steamvr", "system"])
    ap.add_argument("--seconds", type=float, default=8, help="settle time after equipping, before the measurements")
    ap.add_argument("--perf-seconds", type=float, default=6)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-weapon", action="store_true")
    ap.add_argument("--no-body", action="store_true", help="no body flag: the shipped smooth-only mode (control run)")
    ap.add_argument("--no-deploy", action="store_true", help="run the deployed DLLs as they are")
    ap.add_argument("--look-down", type=float, default=-55.0, help="HMD pitch for the body capture (deg, negative = down)")
    ap.add_argument("--motion", action="store_true", help="move the right controller 20 cm and back (hand follow check)")
    ap.add_argument("--unequip", action="store_true", help="unequip the 10mm before the captures (no weapon model)")
    ap.add_argument("--outfit", default="", help="ARMO form id to add + equip (e.g. 1fe1a6 Maxson's battlecoat: long skirt panels)")
    ap.add_argument("--turns", action="store_true", help="snap turns, smooth turn, physical head yaw; FRIK heading lines")
    ap.add_argument("--selfie", action="store_true", help="FRIK selfie mode (body projected in front) via a test flag file")
    ap.add_argument("--head-up", type=float, default=1.0, help="HMD height above the recentred pose (m)")
    ap.add_argument("--cmd", action="append", default=[], help="pipe command(s) to run in world")
    o = ap.parse_args()
    why = busy()
    if why:
        raise SystemExit(f"not now: {why}")
    if o.xr == "steamvr" and vremu("/state").startswith("vremu error"):
        # 15 Nov rule: never start/close/restart SteamVR from here; fo4test refuses a SteamVR launch without vremu anyway
        raise SystemExit("not now: vremu not answering on 127.0.0.1:8766 (SteamVR not in emulator mode / not running)")
    os.makedirs(OUT, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    frik_log = os.path.join(LOGDIR, "FRIK.log")
    if os.path.exists(frik_log):
        os.replace(frik_log, frik_log + f".before-{stamp}")  # same folder: a rename, never a delete
    if not o.no_deploy:
        deploy()
    if not o.no_body:
        with open(BODY_FLAG, "w") as fh:  # full FRIK-flat for this run only; renamed away in the finally below
            fh.write("frik_session.py body test\n")
    selfie_flag = os.path.join(PLUG, "fo4flatvr_frik_selfie.test")
    if o.selfie:
        with open(selfie_flag, "w") as fh:
            fh.write("frik_session.py selfie test\n")
    result = {"stamp": stamp}
    try:
        kw = {"no_xr": True} if o.xr == "none" else ({} if o.xr == "system" else {"runtime_json": STEAMVR_JSON})
        with fo4test.Game(frik=True, **kw) as g:
            to_world(g)
            if o.xr == "steamvr":
                vremu("/pose?dev=hmd&pos=0,1.7,0&ypr=0,0,0")
                vremu("/pose?dev=right&pos=0.2,1.35,-0.35&ypr=0,0,0")
                vremu("/pose?dev=left&pos=-0.2,1.35,-0.35&ypr=0,0,0")
                time.sleep(0.5)
                say(f"recenter: {g.cmd('recenter', quiet=True)[:80]}")
                time.sleep(0.5)
                # head above the recentred pose (the deployed fHeightOffsetCm=-100 puts the eye 70 u below the game's): default
                # +1.0 m = eye at the game's own eye height (~120 u), FO4VR standing
                hy = 1.7 + o.head_up
                vremu(f"/pose?dev=hmd&pos=0,{hy:.2f},0&ypr=0,0,0")
                vremu(f"/pose?dev=right&pos=0.2,{hy - 0.45:.2f},-0.35&ypr=0,0,0")
                vremu(f"/pose?dev=left&pos=-0.2,{hy - 0.45:.2f},-0.35&ypr=0,0,0")
            if o.outfit:
                g.cmd(f"console player.additem {o.outfit} 1")
                time.sleep(0.5)
                g.cmd(f"console player.equipitem {o.outfit}")
                time.sleep(2.0)
            if not o.no_weapon:
                g.cmd("console player.additem 00004822 1")
                g.cmd("console player.additem 0001F276 200")
                time.sleep(1.0)
                say(f"equip: {g.cmd('aim equip 00004822')}")
            t0 = time.time()
            while time.time() - t0 < o.seconds and fo4test.game_pid():
                time.sleep(0.5)
            say(f"frik: {g.cmd('frik', quiet=True)}")
            say(f"gun: {g.cmd('gun', quiet=True)[:300]}")
            # frame time: FRIK frame on vs skipped (same scene, same session); then the captures
            for label, cb in (("on", "cb on"), ("off", "cb off"), ("on2", "cb on")):
                g.cmd("frik " + cb, quiet=True)
                time.sleep(1.0)
                g.cmd("frik perf", quiet=True)  # reset the window
                time.sleep(o.perf_seconds)
                result["perf_" + label] = g.cmd("frik perf", quiet=True)
                say(f"perf body {label}: {result['perf_' + label]}")
            g.cmd("frik cb on", quiet=True)
            if o.motion and o.xr == "steamvr":
                hy = 1.7 + o.head_up
                say("motion: right controller +20 cm right (expect the third-person right hand to follow ~14 u)")
                vremu(f"/pose?dev=right&pos=0.4,{hy - 0.45:.2f},-0.35&ypr=0,0,0")
                time.sleep(2.2)
                vremu(f"/pose?dev=right&pos=0.2,{hy - 0.45:.2f},-0.35&ypr=0,0,0")
                time.sleep(1.2)
            if o.turns and o.xr == "steamvr":
                hy = 1.7 + o.head_up
                def mark(label):
                    time.sleep(1.6)  # FRIK's heading diag line comes once a second
                    say(f"TURN {label} @ {time.strftime('%H:%M:%S')}")
                mark("start")
                say(f"comfort: {g.cmd('comfort turn snap', quiet=True)}")
                for _ in range(6):  # snap pulses (6 x 30 deg = 180) (stick right, release to re-arm)
                    vremu("/input?dev=right&joy=1,0&ms=150")
                    time.sleep(0.45)
                mark("after 6 snap pulses")
                say(f"comfort: {g.cmd('comfort turn smooth', quiet=True)}")
                vremu("/input?dev=right&joy=1,0&ms=1500")
                time.sleep(1.6)
                mark("after 1.5 s smooth turn")
                vremu(f"/pose?dev=hmd&pos=0,{hy:.2f},0&ypr=60,0,0")
                mark("physical head yaw 60")
                vremu(f"/pose?dev=hmd&pos=0,{hy:.2f},0&ypr=0,0,0")
                mark("physical head back to 0")
                say(f"comfort status: {g.cmd('comfort', quiet=True)[:300]}")
            if o.unequip:
                say(f"unequip: {g.cmd('console player.unequipitem 00004822', quiet=True)}")
                time.sleep(3.0)
            gs = g.cmd("gun", quiet=True)
            i = gs.find("wWorld=")
            say(f"gun after: aside/gate: {gs[gs.find('aside='):gs.find('aside=') + 40]} | {gs[i:i + 120] if i >= 0 else '-'}")
            for c in o.cmd:
                say(f"cmd {c}: {g.cmd(c)}")
            if fo4test.game_pid():
                result["shot_fwd"] = capture(g, f"body-{stamp}-fwd")
                if o.xr == "steamvr":
                    vremu(f"/pose?dev=hmd&pos=0,{1.7 + o.head_up:.2f},0&ypr=0,{o.look_down},0")
                    time.sleep(1.5)
                    result["shot_down"] = capture(g, f"body-{stamp}-down", both=True)
                    vremu(f"/pose?dev=hmd&pos=0,{1.7 + o.head_up:.2f},0&ypr=0,0,0")
                result["status"] = g.status().get("state")
            result["alive"] = bool(fo4test.game_pid())
    finally:
        if os.path.exists(selfie_flag):
            os.replace(selfie_flag, selfie_flag + ".off")
        undeploy()  # body flag off; FRIK.dll itself stays (shipped since 15 Nov in smooth-movement-only mode)
        if os.path.exists(frik_log):
            shutil.copy2(frik_log, os.path.join(OUT, f"FRIK-{stamp}.log"))
            body = [l.rstrip() for l in open(frik_log, encoding="utf-8", errors="replace") if "FRIK-flat body:" in l]
            for l in body[-4:]:
                say(l[:400])
        flog = fo4test.LOG
        if os.path.exists(flog):
            lines = [l for l in open(flog, encoding="utf-8", errors="replace") if "[FRIK]" in l or "gun: placement" in l]
            open(os.path.join(OUT, f"fo4flatvr-FRIK-{stamp}.txt"), "w", encoding="utf-8").writelines(lines)
    say(f"result {result}; logs in {OUT}")


if __name__ == "__main__":
    main()
