#!/usr/bin/env python3
"""
extract_fission_products.py
---------------------------
Reads all Serpent depletion (_dep.m) segments of ONE case, stitches them along
the time axis, and writes per-nuclide mass tables in the same
"Step / Day [d] / <nuclide>_kg" column format your Cyclus ADS loader already
reads (i.e. like plot_data_06_actinide_masses.txt).

It writes two files into the case folder:
  plot_data_14_fission_product_masses.txt   (Z in [zmin, zmax], default 28..72)
  plot_data_15_actinide_masses_full.txt     (Z >= 89, ALL actinides)

Why a Z-window for fission products: the _dep.m "total" includes the coolant/
salt (Li, Cl, F, Na, K) and, in the Pb cases, lead. Selecting Z in [28,72]
brackets both fission-yield humps and cleanly excludes salt (Z<=19), Pb (82),
and the actinides (>=89).

Segments are ordered by their first DAY (robust to the confusing part-number
filenames), and the duplicated boundary time-point shared by consecutive
restart segments is dropped.

Usage:
  python3 extract_fission_products.py /path/to/long_no_source_500MW_..._flinak
  python3 extract_fission_products.py CASE_DIR --zmin 28 --zmax 72
"""

import argparse
import glob
import os
import re
import sys

SYM = {1:"H",2:"He",3:"Li",4:"Be",5:"B",6:"C",7:"N",8:"O",9:"F",10:"Ne",
11:"Na",12:"Mg",13:"Al",14:"Si",15:"P",16:"S",17:"Cl",18:"Ar",19:"K",20:"Ca",
21:"Sc",22:"Ti",23:"V",24:"Cr",25:"Mn",26:"Fe",27:"Co",28:"Ni",29:"Cu",30:"Zn",
31:"Ga",32:"Ge",33:"As",34:"Se",35:"Br",36:"Kr",37:"Rb",38:"Sr",39:"Y",40:"Zr",
41:"Nb",42:"Mo",43:"Tc",44:"Ru",45:"Rh",46:"Pd",47:"Ag",48:"Cd",49:"In",50:"Sn",
51:"Sb",52:"Te",53:"I",54:"Xe",55:"Cs",56:"Ba",57:"La",58:"Ce",59:"Pr",60:"Nd",
61:"Pm",62:"Sm",63:"Eu",64:"Gd",65:"Tb",66:"Dy",67:"Ho",68:"Er",69:"Tm",70:"Yb",
71:"Lu",72:"Hf",73:"Ta",74:"W",75:"Re",76:"Os",77:"Ir",78:"Pt",79:"Au",80:"Hg",
81:"Tl",82:"Pb",83:"Bi",84:"Po",85:"At",86:"Rn",87:"Fr",88:"Ra",89:"Ac",90:"Th",
91:"Pa",92:"U",93:"Np",94:"Pu",95:"Am",96:"Cm",97:"Bk",98:"Cf",99:"Es",100:"Fm",
101:"Md",102:"No",103:"Lr"}


def read_array(text, name):
    """Return the raw text inside 'name = [ ... ]', or None."""
    m = re.search(re.escape(name) + r"\s*=\s*\[(.*?)\]", text, re.DOTALL)
    return m.group(1) if m else None


def zai_split(zai):
    zai = int(zai)
    return zai // 10000, (zai % 10000) // 10, zai % 10  # Z, A, I


def nuc_name(zai):
    Z, A, I = zai_split(zai)
    return f"{SYM.get(Z, 'E'+str(Z))}{A}" + ("m" if I else "")


def parse_dep(path):
    """Parse one _dep.m: returns (zai list, days list, mass matrix rows[g])."""
    txt = open(path, "r", errors="ignore").read()
    zblock = read_array(txt, "ZAI")
    dblock = read_array(txt, "DAYS")
    mblock = read_array(txt, "TOT_MASS")
    if zblock is None or dblock is None or mblock is None:
        raise ValueError(f"{path}: missing ZAI/DAYS/TOT_MASS block")

    zai = [int(x) for x in zblock.split()]
    days = [float(x) for x in dblock.split()]
    rows = []
    for line in mblock.strip().splitlines():
        line = line.split("%")[0].strip()        # drop the "% Cs137" comment
        if not line:
            continue
        rows.append([float(v) for v in line.split()])
    if len(rows) != len(zai):
        raise ValueError(f"{path}: {len(rows)} mass rows vs {len(zai)} ZAI")
    return zai, days, rows


def stitch_case(case_dir):
    """Find all _dep.m under case_dir, order by first day, stitch time axis."""
    files = glob.glob(os.path.join(case_dir, "**", "*_dep.m"), recursive=True)
    files += glob.glob(os.path.join(case_dir, "**", "*.inp_dep.m"), recursive=True)
    files = sorted(set(files))
    if not files:
        sys.exit(f"error: no *_dep.m files found under {case_dir}")

    segments = []
    ref_zai = None
    for f in files:
        try:
            zai, days, rows = parse_dep(f)
        except ValueError as e:
            print(f"  [skip] {e}")
            continue
        if ref_zai is None:
            ref_zai = zai
        elif zai != ref_zai:
            print(f"  [warn] {os.path.basename(f)} has a different ZAI list; "
                  "skipping (nuclide sets must match to stitch).")
            continue
        segments.append((days[0], days, rows, f))

    if not segments:
        sys.exit("error: no parseable segments.")
    segments.sort(key=lambda s: s[0])

    # Concatenate, dropping a segment's first point if it duplicates the
    # previous segment's last day (Serpent restart boundary).
    all_days = []
    all_cols = []          # list of columns, each column = mass vector over nuclides
    last_day = None
    for _, days, rows, f in segments:
        ncol = len(rows[0])
        for k in range(ncol):
            if last_day is not None and abs(days[k] - last_day) < 1e-9:
                continue   # duplicate boundary point
            all_days.append(days[k])
            all_cols.append([row[k] for row in rows])
            last_day = days[k]

    print(f"  stitched {len(segments)} segment(s) -> {len(all_days)} time points "
          f"({all_days[0]:.0f} .. {all_days[-1]:.0f} days)")
    return ref_zai, all_days, all_cols


def write_table(path, zai, days, cols, keep):
    """Write Step/Day/<nuc>_kg table for nuclides whose Z passes keep(Z).
       cols[t] is the per-nuclide gram vector at time t (aligned to zai)."""
    # choose nuclide rows (skip pseudo 666/0 and zero-everywhere)
    idx, names = [], []
    for i, z in enumerate(zai):
        if z in (666, 0):
            continue
        Z, A, I = zai_split(z)
        if not keep(Z):
            continue
        if all(cols[t][i] <= 0.0 for t in range(len(days))):
            continue
        idx.append(i)
        names.append(nuc_name(z))

    with open(path, "w") as fh:
        fh.write("# Generated by extract_fission_products.py from Serpent _dep.m\n")
        fh.write("# Masses in kg (Serpent TOT_MASS grams / 1000).\n")
        fh.write("-" * 90 + "\n")
        fh.write(" Step         Day [d]     " +
                 "   ".join(n + "_kg" for n in names) + "\n")
        fh.write("-" * 90 + "\n")
        for t, day in enumerate(days):
            vals = "   ".join(f"{cols[t][i]/1000.0:.6E}" for i in idx)
            fh.write(f"{t+1:5d}   {day:14.4f}   {vals}\n")
        fh.write("-" * 90 + "\n")
    return len(names)


def summary(zai, days, cols, zmin, zmax):
    tot = {"actinide": 0.0, "fp": 0.0, "other": 0.0}
    last = cols[-1]
    for i, z in enumerate(zai):
        if z in (666, 0):
            continue
        Z, _, _ = zai_split(z)
        if Z >= 89:
            tot["actinide"] += last[i]
        elif zmin <= Z <= zmax:
            tot["fp"] += last[i]
        else:
            tot["other"] += last[i]
    print(f"  at day {days[-1]:.0f}:  actinides={tot['actinide']/1000:.1f} kg  "
          f"fission_products={tot['fp']/1000:.1f} kg  "
          f"coolant/other={tot['other']/1000:.1f} kg")


def main():
    ap = argparse.ArgumentParser(description="Extract stitched FP/actinide mass tables from Serpent _dep.m")
    ap.add_argument("case_dir", help="case folder containing run_*/*_dep.m")
    ap.add_argument("--zmin", type=int, default=28, help="min Z counted as fission product")
    ap.add_argument("--zmax", type=int, default=72, help="max Z counted as fission product")
    args = ap.parse_args()

    print(f"Case: {args.case_dir}")
    zai, days, cols = stitch_case(args.case_dir)
    summary(zai, days, cols, args.zmin, args.zmax)

    fp_path = os.path.join(args.case_dir, "plot_data_14_fission_product_masses.txt")
    ac_path = os.path.join(args.case_dir, "plot_data_15_actinide_masses_full.txt")
    n_fp = write_table(fp_path, zai, days, cols,
                       keep=lambda Z: args.zmin <= Z <= args.zmax)
    n_ac = write_table(ac_path, zai, days, cols, keep=lambda Z: Z >= 89)
    print(f"  wrote {n_fp} fission-product columns -> {fp_path}")
    print(f"  wrote {n_ac} actinide columns        -> {ac_path}")


if __name__ == "__main__":
    main()
