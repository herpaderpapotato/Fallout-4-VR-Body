"""FRIK-flat wrist invariant test (C32): with the HMD and both controllers still in tracking space, each hand's rotation relative
to its controller must not change with the world heading.

Deploys only FRIK.dll into the FRIK-flat MO2 mod folder (the deployed one is backed up to work/deploy-backup and put back
afterwards unless --keep-deployed), launch through MO2 (fo4test, both mods must be enabled), body test flag like
frik_session.py, SteamVR + vremu only, `vrtest_sanctuary`. Phases: as loaded (no weapon drawn), 10mm drawn, after unequipping
it. Per phase: snap turns to +0/+90/+180/+270, a smooth turn, physical turns of the whole tracked rig (HMD + controllers)
by 90/180/270 deg. FRIK.log `FRIK-flat wrist:` lines (1/s) give per hand the rotation of each chain stage relative to the raw
grip; the table prints how far each stage turned (deg) against the phase's first heading.

Usage: python -I frik-flat/tools/wrist_session.py [--dll <FRIK.dll>] [--label x] [--keep-deployed] [--phases none,gun,unequip]
Refuses while Fallout4 runs, work/fo4test.hold exists or an xmake build runs. Never starts/closes SteamVR.
"""
import argparse
import math
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import frik_session as fs  # noqa: E402

fo4test = fs.fo4test
PLUG = fs.PLUG
FRIK_PLUG = fs.FRIK_PLUG  # FRIK-flat MO2 mod folder (F4SE/Plugins)
OUT = fs.OUT
FRIK_LOG = os.path.join(fs.LOGDIR, "FRIK.log")

# tilted controllers (a pure-yaw grip would hide a heading rotation applied in the wrong frame: yaw rotations commute)
LEFT = ((-0.20, -0.45, -0.35), (15.0, -35.0, 20.0))
RIGHT = ((0.20, -0.45, -0.35), (-15.0, -35.0, -20.0))
RIGHT_IDENT = ((0.20, -0.45, -0.35), (0.0, 0.0, 0.0))


def rot_y(p, deg):
    a = math.radians(deg)
    x, y, z = p
    return (x * math.cos(a) + z * math.sin(a), y, -x * math.sin(a) + z * math.cos(a))


def pose_rig(hy, yaw, right=RIGHT):
    """HMD at (0, hy, 0) yawed by `yaw`; controllers rigidly attached to it (positions rotated about the head, yaw added)."""
    fs.vremu(f"/pose?dev=hmd&pos=0,{hy:.3f},0&ypr={yaw:.1f},0,0")
    for dev, (p, ypr) in (("left", LEFT), ("right", right)):
        q = rot_y(p, yaw)
        fs.vremu(f"/pose?dev={dev}&pos={q[0]:.4f},{hy + q[1]:.4f},{q[2]:.4f}&ypr={ypr[0] + yaw:.1f},{ypr[1]:.1f},{ypr[2]:.1f}")


def snap(n):
    for _ in range(n):
        fs.vremu("/input?dev=right&joy=1,0&ms=150")
        time.sleep(0.45)


# ---- log parsing -------------------------------------------------------------------------------------------------------------
STAGES = ("wand", "off", "tgt", "node", "tree", "fa3")
V3 = r"\((-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)\)"


def mat_from_rv(v):
    x, y, z = (math.radians(c) for c in v)
    a = math.sqrt(x * x + y * y + z * z)
    if a < 1e-9:
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    x, y, z = x / a, y / a, z / a
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    return [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
            [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
            [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]


def angle_between(a, b):
    ma, mb = mat_from_rv(a), mat_from_rv(b)
    tr = sum(ma[k][i] * mb[k][i] for i in range(3) for k in range(3))  # trace(ma^T mb)
    return math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1) / 2))))


def parse_line(line):
    m = re.search(r"^(\d\d):(\d\d):(\d\d)\.(\d+).*FRIK-flat wrist: cam (-?[\d.]+) body (-?[\d.]+) actor (-?[\d.]+) drawn (\d) unarmed (\d)(.*)$", line)
    if not m:
        return None
    t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + float("0." + m.group(4))
    r = {"t": t, "cam": float(m.group(5)), "body": float(m.group(6)), "drawn": int(m.group(8)), "unarmed": int(m.group(9))}
    rest = m.group(10)
    for side in ("L", "R"):
        sm = re.search(r"(?:^|\| )" + side + r" (wand.*?)(?= \| R |$)", rest)
        if not sm:
            continue
        seg = sm.group(1)
        d = {}
        for st in STAGES:
            vm = re.search(r"\b" + st + V3, seg)
            if vm:
                d[st] = tuple(float(vm.group(i)) for i in (1, 2, 3))
        am = re.search(r"ax (.*)$", seg)
        d["ax"] = am.group(1) if am else "-"
        r[side] = d
    return r


def secs(ts):
    lt = time.localtime(ts)
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + (ts % 1)


def pick(lines, t0, t1):
    """last wrist line in [t0, t1] (seconds of the day)"""
    best = None
    for r in lines:
        if t0 <= r["t"] <= t1:
            best = r
    return best


def table(rows):
    """rows: [(phase, label, rec)] -> printed table: per phase, deg each stage turned vs the phase's first row"""
    out = []
    hdr = f"{'phase':8} {'step':16} {'cam':>7} {'body-cam':>8} | " + " ".join(f"L{s:>4}" for s in STAGES) + " | " + " ".join(f"R{s:>4}" for s in STAGES)
    out.append(hdr)
    base = {}
    for phase, label, r in rows:
        if r is None:
            out.append(f"{phase:8} {label:16} (no line)")
            continue
        if phase not in base:
            base[phase] = r
        b = base[phase]
        cells = []
        for side in ("L", "R"):
            for st in STAGES:
                if side in r and side in b and st in r[side] and st in b[side]:
                    cells.append(f"{angle_between(b[side][st], r[side][st]):5.1f}")
                else:
                    cells.append(f"{'-':>5}")
        bc = (r["body"] - r["cam"] + 180) % 360 - 180
        out.append(f"{phase:8} {label:16} {r['cam']:7.1f} {bc:8.1f} | " + " ".join(cells[:6]) + " | " + " ".join(cells[6:]) +
                   f"  drawn={r['drawn']} unarmed={r['unarmed']}")
    return "\n".join(out)


def main():
    os.environ["FO4FLATVR_FRIK_BODY"] = "1"
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", default=os.path.join(fs.FRIK_BUILD, "FRIK.dll"))
    ap.add_argument("--label", default="")
    ap.add_argument("--keep-deployed", action="store_true", help="leave --dll deployed (else the previously deployed FRIK.dll is put back)")
    ap.add_argument("--phases", default="none,gun,unequip")
    ap.add_argument("--head-up", type=float, default=1.0)
    ap.add_argument("--parse", default="", help="only parse an existing FRIK log + marks file (no launch)")
    o = ap.parse_args()
    if o.parse:
        return report(o.parse, o.parse.replace(".log", ".marks.txt"))
    why = fs.busy()
    if why:
        raise SystemExit(f"not now: {why}")
    if fs.vremu("/state").startswith("vremu error"):
        raise SystemExit("not now: vremu not answering on 127.0.0.1:8766")
    os.makedirs(OUT, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    tag = f"wrist-{stamp}{('-' + o.label) if o.label else ''}"
    if os.path.exists(FRIK_LOG):
        os.replace(FRIK_LOG, FRIK_LOG + f".before-{stamp}")
    dep = os.path.join(FRIK_PLUG, "FRIK.dll")
    os.makedirs(fo4test.DEPLOY_BACKUP, exist_ok=True)
    saved = os.path.join(fo4test.DEPLOY_BACKUP, f"FRIK.dll.{time.strftime('%Y%m%d-%H%M%S')}-wristtest-restore")
    shutil.copy2(dep, saved)  # copy of the deployed DLL (put back in finally)
    fo4test.deploy_files([(o.dll, "FRIK.dll")], FRIK_PLUG, "wristtest")  # MO2 mod folder; refuses while a game/xmake runs
    fs.say(f"FRIK.dll <- {o.dll} (deployed one saved as {saved})")
    with open(fs.BODY_FLAG, "w") as fh:
        fh.write("wrist_session.py body test\n")
    marks = []

    def mark(phase, label):
        time.sleep(2.6)  # settle + at least two 1/s diag lines
        marks.append((phase, label, time.time() - 1.5, time.time()))
        fs.say(f"MARK {phase} {label}")

    try:
        with fo4test.Game(runtime_json=fs.STEAMVR_JSON, frik=True) as g:
            fs.to_world(g)
            hy0 = 1.7
            pose_rig(hy0, 0.0)
            time.sleep(0.5)
            fs.say(f"recenter: {g.cmd('recenter', quiet=True)[:80]}")
            time.sleep(0.5)
            hy = hy0 + o.head_up
            pose_rig(hy, 0.0)
            comfort0 = g.cmd("comfort", quiet=True)
            fs.say(f"comfort before: {comfort0[:200]}")
            g.cmd("console player.additem 00004822 1", quiet=True)
            g.cmd("console player.additem 0001F276 200", quiet=True)
            time.sleep(4.0)
            for phase in o.phases.split(","):
                if phase == "gun":
                    fs.say(f"equip: {g.cmd('aim equip 00004822', quiet=True)}")
                    time.sleep(4.0)
                elif phase == "unequip":
                    fs.say(f"unequip: {g.cmd('console player.unequipitem 00004822', quiet=True)}")
                    time.sleep(4.0)
                pose_rig(hy, 0.0)
                g.cmd("comfort turn snap", quiet=True)
                mark(phase, "snap +0")
                for k in (1, 2, 3):
                    snap(3)
                    mark(phase, f"snap +{90 * k}")
                snap(3)  # back to +360
                g.cmd("comfort turn smooth", quiet=True)
                fs.vremu("/input?dev=right&joy=1,0&ms=700")
                time.sleep(0.8)
                mark(phase, "smooth turn")
                g.cmd("comfort turn snap", quiet=True)
                for yaw in (90.0, 180.0, 270.0):
                    pose_rig(hy, yaw)
                    mark(phase, f"physical {yaw:.0f}")
                pose_rig(hy, 0.0)
                if phase == "none":
                    pose_rig(hy, 0.0, right=RIGHT_IDENT)
                    mark(phase, "R grip identity")
                    pose_rig(hy, 0.0)
            # restore the comfort turn mode seen at the start
            m = re.search(r"turn=(\w+)", comfort0)
            if m and m.group(1) in ("snap", "smooth", "off", "game"):
                g.cmd(f"comfort turn {m.group(1)}", quiet=True)
    finally:
        if os.path.exists(fs.BODY_FLAG):
            os.replace(fs.BODY_FLAG, fs.BODY_FLAG + ".off")
        if not fo4test.game_pid() and not o.keep_deployed:
            fo4test.deploy_files([(saved, "FRIK.dll")], FRIK_PLUG, "tested")  # tested DLL -> work/deploy-backup
            fs.say("deployed FRIK.dll put back")
        log_copy = os.path.join(OUT, f"FRIK-{tag}.log")
        if os.path.exists(FRIK_LOG):
            shutil.copy2(FRIK_LOG, log_copy)
        mfile = log_copy.replace(".log", ".marks.txt")
        with open(mfile, "w") as fh:
            for p, label, t0, t1 in marks:
                fh.write(f"{p}\t{label}\t{secs(t0):.3f}\t{secs(t1):.3f}\n")
    report(log_copy, mfile)


def report(log_path, marks_path):
    lines = [r for r in (parse_line(l.rstrip()) for l in open(log_path, encoding="utf-8", errors="replace")) if r]
    rows = []
    for l in open(marks_path):
        p, label, t0, t1 = l.rstrip("\n").split("\t")
        rows.append((p, label, pick(lines, float(t0), float(t1))))
    txt = table(rows)
    print(txt)
    print("\nrel vectors (deg) of the first row per phase, and the axes lines:")
    seen = set()
    for p, label, r in rows:
        if r and (p not in seen or label == "R grip identity"):
            seen.add(p)
            for side in ("L", "R"):
                if side in r:
                    print(f"  {p} {label} {side}: " + " ".join(f"{s}{r[side].get(s)}" for s in STAGES) + f" ax {r[side]['ax']}")
    with open(log_path.replace(".log", ".table.txt"), "w") as fh:
        fh.write(txt + "\n")


if __name__ == "__main__":
    main()
