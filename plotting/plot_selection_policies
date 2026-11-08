#!/usr/bin/env python3
"""
policy_compare.py  --  Offline comparison of us_inventory selection policies.

Reproduces the draw order that the `us_inventory` Cyclus archetype would produce
for a given selection_policy, WITHOUT running Cyclus. This is exact because the
archetype's ChooseBin_ scans the pool and pops the single best remaining bin on
every trade -- which is identical to a stable sort of the whole pool by the
policy key, with ties broken by dataset order.

It then computes the policy-dependent observables that actually differ
(trajectory, timing, provenance) and the policy-invariant one (final totals),
and writes two comparison figures plus a printed summary table.

Semantics matched to us_inventory.cc:
  minor actinide = grams of Np(Z=93) + Am(Z=95) + Cm(Z=96)
  ma_frac        = MA grams / total grams        (policies sort on THIS)
  burnup         = max_burnup_mwd_per_mthm
  older / newer  = ascending / descending discharge_year
  first          = dataset order (sorted filenames, then line order)
  assembly mass  = sum of all nuclide grams / 1000  (kg)

Usage:
  # Version A: MA-driven vs neutral baseline
  python3 policy_compare.py DATA_DIR --set A -o policy_figs_A

  # Version B: age- and burnup-driven
  python3 policy_compare.py DATA_DIR --set B -o policy_figs_B

  # or name policies explicitly:
  python3 policy_compare.py DATA_DIR \
      --policies highest_minor_actinide,lowest_minor_actinide,first -o out

DATA_DIR may be a folder of STANDARDS JSONL files or a single file.
"""

import argparse
import ast
import csv
import glob
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- element symbol -> Z (only what we need for MA / Pu / fissile) -----------
_Z = {"np": 93, "pu": 94, "am": 95, "cm": 96}
_FISSILE = {("u", 233), ("u", 235), ("pu", 239), ("pu", 241)}

# Preset policy groups the user asked for.
SET_A = ["highest_minor_actinide", "lowest_minor_actinide", "first"]
SET_B = ["older", "newer", "highest_burnup", "lowest_burnup"]

# Sort specification per policy: (key_function, descending?)
#   descending is implemented by sorting on the negated key so that ties keep
#   dataset order (Python's stable sort + reverse=True would invert ties).
def _sort_spec(policy):
    if policy == "first":
        return (lambda a: a["order"], False)
    if policy == "older":
        return (lambda a: a["year"], False)      # ascending year = oldest first
    if policy == "newer":
        return (lambda a: -a["year"], False)      # descending year = newest first
    if policy == "highest_burnup":
        return (lambda a: -a["burnup"], False)
    if policy == "lowest_burnup":
        return (lambda a: a["burnup"], False)
    if policy == "highest_minor_actinide":
        return (lambda a: -a["ma_frac"], False)
    if policy == "lowest_minor_actinide":
        return (lambda a: a["ma_frac"], False)
    raise ValueError("unsupported policy: " + policy)


# ---- parsing -----------------------------------------------------------------
def parse_file(path, order_start):
    """Parse one STANDARDS row_to_json dump into a list of assembly dicts."""
    out = []
    order = order_start
    with open(path, newline="") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row:
                continue
            cell = row[0]
            if "{" not in cell:          # header line 'row_to_json'
                continue
            try:
                d = ast.literal_eval(cell)
            except (ValueError, SyntaxError):
                continue
            grams = d.get("grams", {})
            total_g = 0.0
            ma_g = 0.0
            pu_g = 0.0
            fis_g = 0.0
            for nuc, val in grams.items():
                try:
                    g = float(val)
                except (TypeError, ValueError):
                    continue
                if g <= 0.0:
                    continue
                total_g += g
                sym, _, a = nuc.partition("-")
                z = _Z.get(sym)
                if z in (93, 95, 96):
                    ma_g += g
                elif z == 94:
                    pu_g += g
                try:
                    if (sym, int(a)) in _FISSILE:
                        fis_g += g
                except ValueError:
                    pass
            if total_g <= 0.0:
                continue
            out.append({
                "facility": d.get("facility_name", "?"),
                "location": d.get("storage_location_name", "?"),
                "storage": d.get("storage_type_name", "?"),
                "assembly_kg": total_g / 1000.0,
                "ma_kg": ma_g / 1000.0,
                "ma_frac": ma_g / total_g,
                "pu_frac": pu_g / total_g,
                "burnup": float(d.get("max_burnup_mwd_per_mthm") or 0.0),
                "year": float(d.get("discharge_year") or 0.0),
                "order": order,
            })
            order += 1
    return out, order


def load_dataset(data_dir):
    if os.path.isfile(data_dir):
        files = [data_dir]
    else:
        files = sorted(glob.glob(os.path.join(data_dir, "*.txt")) +
                       glob.glob(os.path.join(data_dir, "*.jsonl")))
    if not files:
        sys.exit("no .txt/.jsonl files found in " + data_dir)
    assemblies = []
    order = 0
    for fp in files:
        recs, order = parse_file(fp, order)
        assemblies.extend(recs)
    return assemblies, files


# ---- analysis ----------------------------------------------------------------
def ordered_draw(assemblies, policy):
    keyfn, _ = _sort_spec(policy)
    # stable sort; dataset order already encoded in 'order' as final tiebreak
    return sorted(assemblies, key=keyfn)


def cumulative(draw):
    cma, casm = [], []
    s_ma = s_asm = 0.0
    for a in draw:
        s_ma += a["ma_kg"]
        s_asm += a["assembly_kg"]
        cma.append(s_ma)
        casm.append(s_asm)
    return cma, casm


def assemblies_to_core(cma, core_ma_kg):
    for i, v in enumerate(cma):
        if v >= core_ma_kg:
            return i + 1
    return None


def facility_contrib_to_core(draw, core_ma_kg):
    """MA (kg) contributed by each facility up to the first full core."""
    contrib = {}
    s = 0.0
    for a in draw:
        contrib[a["facility"]] = contrib.get(a["facility"], 0.0) + a["ma_kg"]
        s += a["ma_kg"]
        if s >= core_ma_kg:
            break
    return contrib


# ---- plotting ----------------------------------------------------------------
def make_figures(assemblies, policies, core_ma_kg, dps, outdir):
    os.makedirs(outdir, exist_ok=True)
    draws = {p: ordered_draw(assemblies, p) for p in policies}
    cums = {p: cumulative(draws[p]) for p in policies}

    total_ma = sum(a["ma_kg"] for a in assemblies)
    total_asm = sum(a["assembly_kg"] for a in assemblies)
    n = len(assemblies)

    # ---- summary table (printed) --------------------------------------------
    print("\n=== dataset ===")
    print(f"  assemblies         : {n}")
    print(f"  total MA           : {total_ma:,.1f} kg")
    print(f"  total assembly mass: {total_asm:,.1f} kg")
    print(f"  core MA target     : {core_ma_kg:,.0f} kg")
    print(f"  days per step      : {dps}")
    print("\n=== per policy (first full core) ===")
    print(f"  {'policy':<26}{'asm→core':>10}{'months':>9}{'years':>8}"
          f"{'#facilities':>13}")
    summary = {}
    for p in policies:
        cma, _ = cums[p]
        k = assemblies_to_core(cma, core_ma_kg)
        contrib = facility_contrib_to_core(draws[p], core_ma_kg)
        if k is None:
            print(f"  {p:<26}{'n/a':>10}{'':>9}{'':>8}{'':>13}   "
                  f"(dataset holds < 1 core of MA)")
            summary[p] = (None, None)
        else:
            months = k * 1.0            # one assembly per trade per step
            print(f"  {p:<26}{k:>10}{months:>9.0f}{months/12.0:>8.1f}"
                  f"{len(contrib):>13}")
            summary[p] = (k, contrib)
    print("\n  NOTE: final cumulative totals are identical across policies by "
          "construction;\n        only the trajectory and provenance differ.\n")

    # ---- Figure 1: MA delivery trajectory ----------------------------------
    fig, ax1 = plt.subplots(figsize=(10, 6))
    for p in policies:
        cma, _ = cums[p]
        x = range(1, len(cma) + 1)
        ax1.plot(x, cma, label=p, lw=1.8)
    ax1.axhline(core_ma_kg, color="k", ls="--", lw=1,
                label=f"1 ADS core ({core_ma_kg/1000:.0f} t MA)")
    # mark where each policy first fills a core
    for p in policies:
        k = summary[p][0]
        if k:
            ax1.plot([k], [core_ma_kg], "o", ms=6)
    ax1.set_ylabel("cumulative MA delivered [kg]")
    ax1.set_xlabel("assemblies drawn  (= timesteps, one assembly per trade)")
    ax1.set_title("Selection policy: MA delivery trajectory\n"
                  "(feed-limited timing differs; endpoint is policy-invariant)")
    ax1.legend(fontsize=9, loc="upper left")
    ax1.grid(alpha=0.3)
    fig.tight_layout()
    f1 = os.path.join(outdir, "policy_trajectory.png")
    fig.savefig(f1, dpi=130)
    plt.close(fig)

    # ---- Figure 2: provenance (facilities filling the first core) ----------
    # union of top facilities across policies
    top = {}
    for p in policies:
        for fac, kg in (summary[p][1] or {}).items():
            top[fac] = max(top.get(fac, 0.0), kg)
    top_facs = [f for f, _ in sorted(top.items(), key=lambda kv: -kv[1])][:12]
    if top_facs:
        fig, ax = plt.subplots(figsize=(11, 6))
        nb = len(policies)
        w = 0.8 / max(nb, 1)
        for j, p in enumerate(policies):
            contrib = summary[p][1] or {}
            vals = [contrib.get(f, 0.0) for f in top_facs]
            xs = [i + (j - (nb - 1) / 2.0) * w for i in range(len(top_facs))]
            ax.bar(xs, vals, width=w, label=p)
        ax.set_xticks(range(len(top_facs)))
        ax.set_xticklabels(top_facs, rotation=40, ha="right", fontsize=8)
        ax.set_ylabel("MA drawn into first core [kg]")
        ax.set_title("Provenance: which sites supply the first ADS core, "
                     "by policy")
        ax.legend(fontsize=8)
        ax.grid(alpha=0.3, axis="y")
        fig.tight_layout()
        f2 = os.path.join(outdir, "policy_provenance.png")
        fig.savefig(f2, dpi=130)
        plt.close(fig)
    else:
        f2 = None

    return f1, f2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir", help="folder of STANDARDS JSONL files, or a file")
    ap.add_argument("--policies", default=None,
                    help="comma-separated policy names")
    ap.add_argument("--set", dest="preset", choices=["A", "B"], default=None,
                    help="A = MA policies + first; B = older/newer/burnup")
    ap.add_argument("--core-ma-kg", type=float, default=27000.0)
    ap.add_argument("--days-per-step", type=float, default=30.4375)
    ap.add_argument("-o", "--outdir", default="policy_figs")
    args = ap.parse_args()

    if args.policies:
        policies = [p.strip() for p in args.policies.split(",") if p.strip()]
    elif args.preset == "A":
        policies = SET_A
    elif args.preset == "B":
        policies = SET_B
    else:
        policies = SET_A

    assemblies, files = load_dataset(args.data_dir)
    print(f"loaded {len(assemblies)} assemblies from {len(files)} file(s)")
    f1, f2 = make_figures(assemblies, policies, args.core_ma_kg,
                          args.days_per_step, args.outdir)
    print("wrote:", f1)
    if f2:
        print("wrote:", f2)


if __name__ == "__main__":
    main()
