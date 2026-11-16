"""Resolve OG address-library ids (what CommonLibF4VR's REL::ID(n) uses) or 1.10.163 RVAs to Fallout 4 1.11.240 RVAs.
og_ids.csv (id -> 1.10.163 rva, VR rva, PDB name) -> ng_ids.csv (og rva shared by a 1.11.240 id) or func_matches.csv.
Usage: python -I resolve.py id:943772 rva:1BA3BE0 ...
"""
import csv
import sys
from pathlib import Path

DB = Path(__file__).resolve().parents[2] / "analysis" / "db"


def main(args):
    ids = {int(a[3:]) for a in args if a.startswith("id:")}
    rvas = {a[4:].upper().lstrip("0") for a in args if a.startswith("rva:")}
    og = {}
    with open(DB / "og_ids.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if int(r["id"]) in ids:
                og[int(r["id"])] = r
                rvas.add(r["rva_1_10_163"].upper().lstrip("0"))
    ng = {}
    with open(DB / "ng_ids.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            k = r["og_rva_1_10_163"].upper().lstrip("0")
            if k in rvas:
                ng.setdefault(k, []).append((r["id"], r["rva_1_11_240"], r["clib_name"]))
    fm = {}
    with open(DB / "func_matches.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            k = r["rva_1_10_163"].upper().lstrip("0")
            if k in rvas:
                fm[k] = (r["rva_1_11_240"], r["confidence"], r["name_1_10_163"])
    for a in args:
        if a.startswith("id:"):
            r = og.get(int(a[3:]))
            if not r:
                print(a, "not in og_ids")
                continue
            k = r["rva_1_10_163"].upper().lstrip("0")
            print(a, "1.10.163", k, "vr", r["vr_rva"], r["pdb_name"][:70], "| ng", ng.get(k), "| fm", fm.get(k))
        else:
            k = a[4:].upper().lstrip("0")
            print(a, "| ng", ng.get(k), "| fm", fm.get(k))


if __name__ == "__main__":
    main(sys.argv[1:])
