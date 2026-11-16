"""FRIK smooth movement on flat (bridge v3, C17): camera-height time series while walking over uneven ground, smoothing
off vs on, in ONE game session (SteamVR + vremu, `load vrtest_sanctuary`, never QASmoke).

    python -I frik-flat/tools/smooth_session.py [--walk-ms 7000] [--runs off,on,off,on] [--keep] [--no-deploy]

Deploys fo4flatvr.dll + FRIK.dll into their MO2 mod folders (frik_session.deploy; smooth-movement-only mode: fo4flatvr.ini [FRIK] bBody absent/0 = code default), launches
through plugin/tools/fo4test.Game, then per run: reload the save, settle, `frik smooth on|off`, hold the virtual pad's left
stick forward for --walk-ms, read `rs camhist` (one row per FirstPersonState update: camera root, capsule loc, ...).
Writes work/frik-flat/smooth-<stamp>/smooth.json (+ per-frame z series, small) and prints the metrics per run.
FRIK.dll stays deployed afterwards (shipped since 15 Nov; --keep has no effect). Launch through MO2 via fo4test.
Refuses while Fallout4 runs, work/fo4test.hold exists or an xmake build runs. Never touches SteamVR.
"""
import argparse
import json
import math
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "plugin", "tools"))
sys.path.insert(0, HERE)
import fo4test  # noqa: E402
import frik_session as fs  # noqa: E402


def camhist(g, n=900):
    r = g.cmd("rs camhist %d" % n, quiet=True)
    out = []
    for part in r.split(" | ")[1:]:
        v = part.split()
        if len(v) < 23:
            continue
        f = [float(x) for x in v]
        out.append(dict(t=f[0], root=f[2:5], loc=f[5:8]))
    return out


def hp(z, w):
    """z minus its centred moving average over w samples (what a 'jitter' is: the part faster than the walk's slope)."""
    out = []
    for i in range(len(z)):
        a, b = max(0, i - w // 2), min(len(z), i + w // 2 + 1)
        out.append(z[i] - sum(z[a:b]) / (b - a))
    return out


def metrics(fr):
    if len(fr) < 20:
        return dict(frames=len(fr))
    t = [f["t"] for f in fr]
    dt = statistics.median([b - a for a, b in zip(t, t[1:])])
    w = max(3, int(round(250.0 / dt)))  # 0.25 s window
    res = dict(frames=len(fr), dt_ms=round(dt, 2), secs=round((t[-1] - t[0]) / 1000.0, 2))
    for key, idx in (("cam", "root"), ("cap", "loc")):
        z = [f[idx][2] for f in fr]
        d1 = [b - a for a, b in zip(z, z[1:])]
        d2 = [c - 2 * b + a for a, b, c in zip(z, z[1:], z[2:])]
        h = hp(z, w)
        res[key] = dict(z_range=round(max(z) - min(z), 2), step_max=round(max(abs(x) for x in d1), 3),
                        step_rms=round(math.sqrt(sum(x * x for x in d1) / len(d1)), 3),
                        acc_rms=round(math.sqrt(sum(x * x for x in d2) / len(d2)), 3),
                        hp_rms=round(math.sqrt(sum(x * x for x in h) / len(h)), 3), hp_max=round(max(abs(x) for x in h), 3))
    # camera - capsule: what the smoothing (plus eye height / head) adds
    rel = [f["root"][2] - f["loc"][2] for f in fr]
    res["cam_minus_cap_z"] = dict(min=round(min(rel), 2), max=round(max(rel), 2))
    lag = [math.hypot(f["root"][0] - f["loc"][0], f["root"][1] - f["loc"][1]) for f in fr]
    res["cam_cap_xy"] = dict(mean=round(sum(lag) / len(lag), 2), max=round(max(lag), 2))
    res["walked_xy"] = round(math.hypot(fr[-1]["loc"][0] - fr[0]["loc"][0], fr[-1]["loc"][1] - fr[0]["loc"][1]), 1)
    return res


def head_steps(g):
    """Physical head step (vremu HMD 20 cm forward, instant and over 1 s), smoothing off vs on, standing still: roomscale
    moves the capsule after the head, and FRIK's horizontal smoothing lags capsule moves. err = camera - room-stable target
    (rs_experiment.overshoot_analyse); a camera that moves without the head shows as err."""
    import rs_experiment as rx
    out = []
    for mode in ("off", "on"):
        g.cmd(f"frik smooth {mode}")
        for ms in (0, 1000):
            e = rx.Exp(g)
            mv = lambda ms=ms: rx.vremu("/pose?dev=hmd&rel=1&x=0&z=-0.2&ms=%d" % ms)
            bk = lambda ms=ms: rx.vremu("/pose?dev=hmd&rel=1&x=0&z=0.2&ms=%d" % ms)
            r = rx.overshoot_step(e, f"step 20cm fwd {ms}ms smooth {mode}", mv, bk, secs=3.0)
            r["frik"] = g.cmd("frik")
            fo4test.say(json.dumps(r))
            out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--walk-ms", type=int, default=6000)
    ap.add_argument("--runs", default="off,on")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-deploy", action="store_true")
    ap.add_argument("--lx", type=float, default=0.0)
    ap.add_argument("--ly", type=float, default=1.0)
    ap.add_argument("--head-steps", action="store_true", default=True)
    ap.add_argument("--no-head-steps", dest="head_steps", action="store_false")
    o = ap.parse_args()
    why = fs.busy()
    if why:
        raise SystemExit(f"not running: {why}")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(fs.OUT, f"smooth-{stamp}")
    os.makedirs(out_dir, exist_ok=True)
    if not o.no_deploy:
        fs.deploy()
    result = dict(stamp=stamp, walk_ms=o.walk_ms, runs=[])
    try:
        with fo4test.Game(runtime_json=fs.STEAMVR_JSON, frik=True) as g:
            mark = "launch"
            fs.to_world(g)
            result["frik_status_start"] = g.cmd("frik")
            result["gun_status_start"] = g.cmd("gun")
            result["nospread_status"] = g.cmd("nospread")
            lg = g.log_since(mark)
            result["log_frik"] = [ln for ln in lg.splitlines() if "[FRIK]" in ln][:12]
            for i, mode in enumerate(o.runs.split(",")):
                g.cmd(f"console load {fs.SAVE}", quiet=True)
                time.sleep(2.0)
                g._wait(f"in-world ({fs.SAVE}) run {i}", 90, lambda: g.ready().get("ingame") == "1")
                time.sleep(3.0)
                g.cmd(f"frik smooth {mode}")
                g.cmd("frik reset", quiet=True)
                time.sleep(1.0)
                t0 = camhist(g, 1)[-1]["t"]
                g.cmd(f"pad lx={o.lx} ly={o.ly} ms={o.walk_ms}")
                time.sleep(o.walk_ms / 1000.0 + 1.5)  # walk + 1.5 s of stopping (FRIK's StoppingMultiplier catch-up)
                fr = [f for f in camhist(g) if f["t"] >= t0]
                st = g.cmd("frik")
                m = metrics(fr)
                m.update(run=i, mode=mode, frik=st)
                result["runs"].append(m)
                fo4test.say(f"run {i} smooth={mode}: {json.dumps({k: m[k] for k in m if k in ('frames', 'walked_xy', 'cam', 'cap', 'cam_cap_xy')})}")
                with open(os.path.join(out_dir, f"run{i}-{mode}.json"), "w", encoding="utf-8") as fh:
                    json.dump([[round(f["t"], 1)] + [round(x, 3) for x in f["root"] + f["loc"]] for f in fr], fh)
            if o.head_steps:
                result["head_steps"] = head_steps(g)
            result["gun_status_end"] = g.cmd("gun")
            result["frik_status_end"] = g.cmd("frik smooth on")  # leave the session as shipped
    finally:
        if not o.keep and not o.no_deploy:
            fs.undeploy()
        with open(os.path.join(out_dir, "smooth.json"), "w", encoding="utf-8") as fh:
            json.dump(result, fh, indent=1)
        fo4test.say(f"wrote {out_dir}")


if __name__ == "__main__":
    main()
