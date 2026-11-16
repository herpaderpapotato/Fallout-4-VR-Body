"""Compile every FRIK-flat source on its own (no PCH, all files, keep going) and tabulate the errors.
xmake stops at the first failing file; this gives the full picture for the Phase 2 log.

Usage: python -I frik-flat/tools/errsweep.py [--filter substr] [--jobs N]
Needs logs/compile_commands.json (xmake project -k compile_commands logs). Writes logs/errsweep.txt.
"""
import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ERR = re.compile(r"^(?P<file>[A-Za-z]:\\[^(]+|[^(:]+)\((?P<line>\d+)(?:,\d+)?\): (?:fatal )?error (?P<code>C\d+): (?P<msg>.*)$")


def args_for(entry):
    a = list(entry["arguments"])
    out = []
    pch = None
    for x in a:
        if x.startswith("/Yu"):
            continue
        if x.startswith("/FI") and x.endswith("PCH.h"):
            pch = "frik-src/PCH.h" if "FRIK" in x else "fw-src/PCH.h"
            continue
        if x.startswith("/Fo"):
            x = "/Fo" + str(ROOT / "logs" / "obj" / (Path(entry["file"]).name + ".obj"))
        if x.startswith("/Fd"):
            x = "/Fd" + str(ROOT / "logs" / "obj" / "sweep.pdb")
        out.append(x)
    if pch:
        out.insert(2, "/FI" + str(ROOT / pch))
    return out


def run(entry):
    r = subprocess.run(args_for(entry), cwd=entry["directory"], capture_output=True, text=True, errors="replace")
    return entry["file"], r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    ap.add_argument("--jobs", type=int, default=16)
    o = ap.parse_args()
    (ROOT / "logs" / "obj").mkdir(parents=True, exist_ok=True)
    cc = json.loads((ROOT / "logs" / "compile_commands.json").read_text())
    todo = [e for e in cc if ("fw-src" in e["file"] or "frik-src" in e["file"] or "vrshim" in e["file"]) and o.filter in e["file"]]
    with ThreadPoolExecutor(o.jobs) as ex:
        res = list(ex.map(run, todo))
    errors = []
    failed = 0
    for f, rc, out in res:
        if rc:
            failed += 1
        for line in out.splitlines():
            m = ERR.match(line.strip())
            if m:
                errors.append((f, m["file"].strip(), int(m["line"]), m["code"], m["msg"].strip()))
    uniq = sorted(set((e[1], e[2], e[3], e[4]) for e in errors))
    lines = [f"files {len(res)}, failed {failed}, error lines {len(errors)}, unique (file,line,code,msg) {len(uniq)}"]
    lines.append("by code: " + str(Counter(u[2] for u in uniq).most_common()))
    byfile = Counter(Path(u[0]).name for u in uniq)
    lines.append("by file: " + str(byfile.most_common(40)))
    msgs = Counter(re.sub(r"'[^']*'", "'X'", u[3]) for u in uniq)
    lines.append("by message shape:")
    lines += [f"  {n:5d} {m}" for m, n in msgs.most_common(60)]
    lines.append("")
    lines += [f"{u[0]}({u[1]}): {u[2]}: {u[3]}" for u in uniq]
    (ROOT / "logs" / "errsweep.txt").write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(lines[:70]))


if __name__ == "__main__":
    main()
