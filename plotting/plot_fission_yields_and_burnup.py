#!/usr/bin/env python3
"""
fp_and_burn.py - two figures for the EINSTEIN thesis.

  11) fission-product yield curve, the classic double hump, built from the
      Serpent plot_data_14_fission_product_masses.txt table
  12) mass loaded vs mass transmuted per cycle, from AdsEvents

Usage
-----
    # both figures
    python3 fp_and_burn.py --case-dir ../serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix \
                           --db puma_recycle_360.sqlite -o figs

    # just the yield curve (no simulation needed)
    python3 fp_and_burn.py --case-dir <case> -o figs

Yield conversion
----------------
Serpent gives fission-product MASS per nuclide.  Atoms are proportional to
mass / A.  Every fission makes two fission products, so

    total atoms = 2 x fissions

and the yield of mass chain A, in percent per fission, is

    Y(A) = 200 * (atoms with mass number A) / (all fission-product atoms)

which integrates to 200% across both humps, exactly like the textbook curve.

This is a CUMULATIVE chain yield taken from the actual in-core inventory, so
it already includes beta decay along each chain and any neutron capture on
fission products.  It is not the independent yield out of a data library, and
at high burnup the two differ noticeably around the strong absorbers.  Say so
in the caption.
"""

import argparse
import os
import re
import sqlite3
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

BLUE, ORANGE, DARK, RED = "#3b6ea5", "#c0662a", "#2c2c2c", "#a5343b"

# long-lived / radiologically important fission products worth labelling
FLAG = {90: "Sr-90", 93: "Zr-93", 99: "Tc-99", 107: "Pd-107", 126: "Sn-126",
        129: "I-129", 135: "Cs-135", 137: "Cs-137", 151: "Sm-151"}


# --------------------------------------------------------------------------
# Serpent table reader - mirrors ads.cc ReadMassTable_
# --------------------------------------------------------------------------
def find_file(case_dir, needle):
    if not os.path.isdir(case_dir):
        sys.exit("no such directory: %s" % case_dir)
    for f in sorted(os.listdir(case_dir)):
        if f.endswith(".txt") and needle in f:
            return os.path.join(case_dir, f)
    return None


ISO = re.compile(r"^([A-Za-z]+)(\d+)m?$")


def parse_iso(label):
    """'Cs137_kg' -> ('Cs', 137).  Returns None if it isn't an isotope."""
    if not label.endswith("_kg") or label.endswith("_delta_kg"):
        return None
    m = ISO.match(label[:-3])
    if not m:
        return None
    a = int(m.group(2))
    if a <= 0 or a > 300:
        return None
    return m.group(1), a


def read_mass_table(path):
    """-> (days list, DataFrame indexed by day with one column per isotope)"""
    header, cols, mapped = None, [], False
    days, rows = [], []
    with open(path) as f:
        for line in f:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            tk = line.split()
            if tk[0] == "Step":
                header = tk
                continue
            if not re.match(r"^\s*\d", line):
                continue
            if not mapped:
                if header is None:
                    sys.exit("%s: no header line starting with 'Step'" % path)
                nval = len(tk) - 2
                off = len(header) - nval
                if off < 0:
                    sys.exit("%s: header/data column mismatch" % path)
                for j in range(nval):
                    iso = parse_iso(header[off + j])
                    if iso:
                        cols.append((2 + j, iso))
                if not cols:
                    sys.exit("%s: no usable '<iso>_kg' columns.\nheader: %s"
                             % (path, " ".join(header)))
                mapped = True
            days.append(float(tk[1]))
            rows.append([float(tk[i]) for i, _ in cols])
    if not rows:
        sys.exit("%s: no data rows" % path)
    names = ["%s%d" % (s, a) for _, (s, a) in cols]
    df = pd.DataFrame(rows, columns=names, index=days)
    amass = {n: a for n, (_, (s, a)) in zip(names, cols)}
    return df, amass


def yield_curve(masses, amass):
    """one row of kg -> (A array, yield % per fission array)"""
    atoms = {}
    for n, kg in masses.items():
        a = amass[n]
        atoms[a] = atoms.get(a, 0.0) + kg / a
    tot = sum(atoms.values())
    if tot <= 0:
        return np.array([]), np.array([])
    A = np.array(sorted(atoms))
    Y = np.array([200.0 * atoms[a] / tot for a in A])
    return A, Y


# --------------------------------------------------------------------------
def fig_yield(case_dir, outdir, days_wanted=None):
    print("fig 11: fission product yield curve")
    path = find_file(case_dir, "fission_product_masses")
    if path is None:
        print("  FAIL: no '*fission_product_masses*.txt' in %s" % case_dir)
        print("        -> this is also why your ADS has no FissionProducts row")
        return
    print("  reading %s" % path)
    df, amass = read_mass_table(path)
    live = df[df.sum(axis=1) > 0]
    if live.empty:
        print("  FAIL: every row is zero")
        return

    if days_wanted is None:
        d = list(live.index)
        days_wanted = sorted({d[len(d) // 6], d[len(d) // 2], d[-1]})

    fig, ax = plt.subplots(2, 1, figsize=(11, 9))
    cmap = plt.get_cmap("viridis")
    for i, day in enumerate(days_wanted):
        j = int(np.argmin(np.abs(np.array(live.index) - day)))
        A, Y = yield_curve(live.iloc[j], amass)
        if not len(A):
            continue
        col = cmap(i / max(1, len(days_wanted) - 1) * .8)
        lab = "%.0f d" % live.index[j]
        ax[0].plot(A, Y, lw=1.8, color=col, label=lab)
        ax[0].fill_between(A, 0, Y, color=col, alpha=.12)
        ax[1].semilogy(A, np.maximum(Y, 1e-6), lw=1.4, color=col, label=lab)

    A, Y = yield_curve(live.iloc[-1], amass)
    ymap = dict(zip(A, Y))
    for a, name in FLAG.items():
        if a in ymap:
            ax[0].plot([a], [ymap[a]], "o", ms=4, color=RED, zorder=5)
            ax[0].annotate(name, xy=(a, ymap[a]), xytext=(0, 7),
                           textcoords="offset points", fontsize=7,
                           ha="center", color=RED)

    ax[0].set_ylabel("chain yield [% per fission]")
    ax[0].set_xlabel("mass number A")
    ax[0].set_title("Fission product yield - %s\n"
                    "cumulative chain yield from the in-core inventory "
                    "(integrates to 200%%: two fragments per fission)"
                    % os.path.basename(os.path.normpath(case_dir)), fontsize=10)
    ax[0].legend(title="burnup time", fontsize=8)
    ax[0].grid(alpha=.3)
    ax[0].set_xlim(min(A) - 2, max(A) + 2)

    ax[1].set_ylabel("chain yield [% per fission] (log)")
    ax[1].set_xlabel("mass number A")
    ax[1].set_title("same, log scale - the symmetric-fission valley", fontsize=10)
    ax[1].legend(fontsize=8)
    ax[1].grid(alpha=.3, which="both")
    ax[1].set_xlim(min(A) - 2, max(A) + 2)

    tot = 200.0
    print("     -> %d mass chains, sum = %.1f%% (should be ~200%%)"
          % (len(A), Y.sum()))
    fig.tight_layout()
    p = os.path.join(outdir, "11_fp_yield_curve.png")
    fig.savefig(p, dpi=140)
    plt.close(fig)
    print("  wrote %s" % p)


def fig_loaded_vs_transmuted(dbpath, outdir):
    print("fig 12: loaded vs transmuted")
    if not os.path.exists(dbpath):
        print("  skip: no such db")
        return
    conn = sqlite3.connect(dbpath)
    have = {r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    if "AdsEvents" not in have:
        print("  skip: no AdsEvents - the ADS never loaded a core")
        return
    sim = conn.execute(
        "SELECT SimId FROM Info ORDER BY rowid DESC LIMIT 1").fetchone()[0]
    ev = pd.read_sql_query(
        "SELECT Time, Event, Value FROM AdsEvents WHERE SimId=?",
        conn, params=(sim,))
    if ev.empty:
        return
    p = ev.pivot_table(index="Time", columns="Event", values="Value",
                       aggfunc="sum", fill_value=0.0)
    for c in ("Load", "Discharge", "ActinideDestroyed", "FissionProducts",
              "MassDefect"):
        if c not in p:
            p[c] = 0.0

    loads = p[p.Load > 0][["Load"]].reset_index()
    disch = p[p.Discharge > 0][["Discharge", "ActinideDestroyed"]].reset_index()
    if disch.empty:
        print("  skip: no discharges yet")
        return

    fig, ax = plt.subplots(3, 1, figsize=(11, 10), sharex=True)

    ax[0].bar(loads.Time, loads.Load, width=2.5, color=BLUE, label="loaded")
    ax[0].bar(disch.Time, disch.ActinideDestroyed, width=2.5, color=ORANGE,
              label="actinide transmuted")
    ax[0].set_ylabel("kg per cycle")
    ax[0].set_title("Per-cycle: charge in vs actinide destroyed")
    ax[0].legend()
    ax[0].grid(alpha=.3, axis="y")

    ax[1].step(loads.Time, loads.Load.cumsum(), where="post", lw=2,
               color=BLUE, label="cumulative loaded")
    ax[1].step(disch.Time, disch.ActinideDestroyed.cumsum(), where="post",
               lw=2, color=ORANGE, label="cumulative transmuted")
    ax[1].fill_between(disch.Time, 0, disch.ActinideDestroyed.cumsum(),
                       step="post", color=ORANGE, alpha=.15)
    ax[1].set_ylabel("cumulative kg")
    ax[1].legend()
    ax[1].grid(alpha=.3)
    if len(disch):
        ax[1].annotate("%.0f kg loaded / %.0f kg transmuted"
                       % (loads.Load.sum(), disch.ActinideDestroyed.sum()),
                       xy=(disch.Time.iloc[-1],
                           disch.ActinideDestroyed.cumsum().iloc[-1]),
                       xytext=(-190, 22), textcoords="offset points",
                       arrowprops=dict(arrowstyle="->"))

    frac = 100.0 * disch.ActinideDestroyed / disch.Discharge
    ax[2].plot(disch.Time, frac, "o-", color=DARK)
    ax[2].set_ylabel("transmuted / loaded  [%]")
    ax[2].set_xlabel("timestep [months]")
    ax[2].set_title("Burn fraction per cycle", fontsize=10)
    ax[2].grid(alpha=.3)
    print("     -> %d loads, %d discharges, mean burn fraction %.1f%%"
          % (len(loads), len(disch), frac.mean()))
    print("     -> NOTE: 'transmuted' is ActinideDestroyed (Z>=89), which lumps")
    print("        MA and Pu together. Where a case breeds Pu out of MA, MA")
    print("        destruction is LARGER than this. For an MA-specific curve")
    print("        use fig 1 of plots.py (--sink for once-through runs).")

    fig.tight_layout()
    q = os.path.join(outdir, "12_loaded_vs_transmuted.png")
    fig.savefig(q, dpi=140)
    plt.close(fig)
    print("  wrote %s" % q)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case-dir", help="Serpent case folder (for fig 11)")
    ap.add_argument("--db", help="cyclus sqlite (for fig 12)")
    ap.add_argument("-o", "--outdir", default="figs")
    ap.add_argument("--days", type=float, nargs="*",
                    help="burnup days to draw on fig 11 (default: 3 spread)")
    a = ap.parse_args()
    if not a.case_dir and not a.db:
        ap.error("give --case-dir and/or --db")
    os.makedirs(a.outdir, exist_ok=True)
    if a.case_dir:
        fig_yield(a.case_dir, a.outdir, a.days)
    if a.db:
        fig_loaded_vs_transmuted(a.db, a.outdir)
    print("\ndone -> %s/" % a.outdir)


if __name__ == "__main__":
    main()