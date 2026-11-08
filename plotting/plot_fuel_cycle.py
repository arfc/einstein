#!/usr/bin/env python3
"""
plots.py - figures from an EINSTEIN / Cyclus run.

Usage
-----
    # Case 4 (50/50 Pu/MA), recycle
    python3 plots.py puma_recycle_360.sqlite -o figs --ref 50.19 49.81

    # Case 3 (MA-only), recycle
    python3 plots.py case3_recycle.sqlite -o figs_r --ref 0 100

    # Case 3 (MA-only), ONCE-THROUGH  <- gives the cleanest MA number
    python3 plots.py case3_once.sqlite -o figs_o --ref 0 100 --sink SpentFuelStore

Notes on the schema this is built against
-----------------------------------------
  * there is NO Resources table; Transactions carries only ResourceId
  * masses come from ExplicitInventory (AgentId, Time, InventoryName,
    NucId, Quantity) - requires <explicit_inventory>1</explicit_inventory>
  * the agent table is AgentEntry, not Agents (Agents is a cymetric view)
  * SimId is a BLOB

Nuclide ids are ZZAAASSSS, so Z = NucId // 10000000.
    minor actinides : Z in (93, 95, 96)   Np, Am, Cm
    plutonium       : Z == 94
    uranium         : Z == 92
    actinides       : Z >= 89      fission products: Z < 89

  NOTE on the "fp" bucket: Z < 89 also catches Tl/Pb/Bi/Po/Ra, which under
  <decay>lazy</decay> are alpha-decay daughters of Am/Cm, not fission
  products. The masses are tiny, but do not call that bucket "fission
  products" in a caption without the caveat.

ExplicitInventory is a SNAPSHOT, not a flow
-------------------------------------------
This is the single most important thing about this schema. A row says how
many kg were sitting in a buffer AT THAT INSTANT. It does not say how much
flowed through. Summing or cumsum-ing a snapshot over time re-counts the
same kg on every step they sit there, which silently inflates the answer by
roughly the residence time (~90x in a 1200-month recycle run).

To turn snapshots into a flow, count the POSITIVE INCREMENTS: see
rise_series() and buffer_inflow() below. This is a lower bound - anything
that arrives and leaves inside one step is invisible - but it is never
inflated, and it is what the sanity checks are for.

How MA destroyed is measured (fig 1)
------------------------------------
The ADS archetype pushes the fresh charge into `core` and does not touch it
until Discharge_ pops + transmutes it, so MA in `core` is exactly MA loaded,
held flat for the whole cycle. For MA out, three sources are tried in order:

  1. --sink NAME   the once-through store. A Sink never gives material back,
                   so its inventory IS cumulative and ffill() is correct.
                   Most reliable - use this when you have it.
  2. the ADS `product` buffer, via positive increments (NOT cumsum).
  3. nothing - only the loaded curve is drawn.

Do NOT use AdsEvents.ActinideDestroyed as "MA destroyed". It counts Z >= 89,
so MA and Pu together. Where a case breeds Pu out of MA, MA destruction is
larger than that number.

AdsEvents naming trap
---------------------
`Discharge` records m_in - the mass popped OUT of the core, i.e. the mass
that was LOADED. It is NOT the discharged mass (that is m_in * mass_ratio,
which is smaller by the mass defect). So:
    ActinideDestroyed / Discharge  ==  destroyed / loaded   <- correct
    SUM(Discharge)                 >=  total mass discharged <- upper bound
The second fact is what fig 1's sanity check leans on.
"""

import argparse
import glob
import os
import sqlite3
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
import numpy as np
import pandas as pd

MA_Z = (93, 95, 96)
PU_Z = 94
U_Z = 92
ACT_Z_MIN = 89

BLUE, ORANGE, DARK, RED = "#3b6ea5", "#c0662a", "#2c2c2c", "#a5343b"
U_COLOR, OTHACT_COLOR, FP_COLOR = "#6a8caf", "#8a5a2b", "#7d7d7d"

SYM = {89: "Ac", 90: "Th", 91: "Pa", 92: "U", 93: "Np", 94: "Pu", 95: "Am",
       96: "Cm", 97: "Bk", 98: "Cf", 36: "Kr", 38: "Sr", 40: "Zr", 42: "Mo",
       43: "Tc", 44: "Ru", 45: "Rh", 46: "Pd", 53: "I", 54: "Xe", 55: "Cs",
       56: "Ba", 57: "La", 58: "Ce", 59: "Pr", 60: "Nd", 61: "Pm", 62: "Sm",
       63: "Eu", 64: "Gd"}

# element symbol -> Z, for parsing "<iso>_kg" column labels in the case files
EL = {v: k for k, v in SYM.items()}


def nucname(n):
    z, a = n // 10000000, (n // 10000) % 1000
    m = "m" if n % 10000 else ""
    return "%s%d%s" % (SYM.get(z, "Z%d" % z), a, m)


def category(n):
    z = n // 10000000
    if z in MA_Z:
        return "MA"
    if z == PU_Z:
        return "Pu"
    if z == U_Z:
        return "U"
    if z >= ACT_Z_MIN:
        return "OtherAct"
    return "FP"


CATCOLOR = {"MA": ORANGE, "Pu": BLUE, "U": U_COLOR,
            "OtherAct": OTHACT_COLOR, "FP": FP_COLOR}


# --------------------------------------------------------------------------
# Serpent case reconstruction (fig 10) - mirrors ads.cc LoadCase_/OutputComp_
#
# Why this exists: the ADS trades its discharge away on `ads_out` in the same
# step it is produced, so it never appears in an ExplicitInventory snapshot,
# and this schema has no Resources table to recover a traded resource's
# composition from its ResourceId. The discharge composition therefore cannot
# be read from the database. It is reconstructed from the matched Serpent case
# named in AdsEvents.MatchedCase: interpolate the actinide + FP mass tables to
# the irradiation day (cycle_time * days_per_step, clamped), scale by the
# loaded mass, times the number of discharges. This is the material the ADS
# puts out BEFORE separations/sink - not the waste bin, which also holds the
# separations reject stream (U, Pu, FP) that never entered the core.
# --------------------------------------------------------------------------
def iso_to_nid(iso):
    """'Am241' -> ZZAAAM id (ground state; metastables fold onto ground)."""
    i = 0
    while i < len(iso) and iso[i].isalpha():
        i += 1
    el, a = iso[:i], iso[i:]
    if el not in EL or not a.isdigit():
        return -1
    return EL[el] * 10000000 + int(a) * 10000


def _find_case_file(case_path, needle):
    for f in glob.glob(os.path.join(case_path, "*.txt")):
        if needle in os.path.basename(f):
            return f
    return ""


def _read_mass_table(path):
    """Mirror ads.cc ReadMassTable_: -> (days list, [ {nid:kg} per row ])."""
    if not path or not os.path.exists(path):
        return [], []
    header, cols, mapped = None, [], False
    days, series = [], []
    for line in open(path):
        if not line.strip() or line[0] == "#":
            continue
        tk = line.split()
        if not tk:
            continue
        if tk[0] == "Step":
            header = tk
            continue
        if not tk[0].lstrip("-").isdigit():
            continue
        if not mapped:
            n_values = len(tk) - 2
            if header and len(header) >= n_values:
                off = len(header) - n_values
                for j in range(n_values):
                    lab = header[off + j]
                    if lab.endswith("_kg") and not lab.endswith("_delta_kg"):
                        nid = iso_to_nid(lab[:-3])
                        if nid > 0:
                            cols.append((2 + j, nid))
            mapped = True
        days.append(float(tk[1]))
        cm = {}
        for idx, nid in cols:
            kg = float(tk[idx])
            if kg > 0:
                cm[nid] = cm.get(nid, 0.0) + kg
        series.append(cm)
    return days, series


def reconstruct_discharge(case_path, target_day):
    """Return (normalized composition {nid: fraction}, mass_ratio) at
    target_day, mirroring ads.cc. (None, None) on failure."""
    act = (_find_case_file(case_path, "actinide_masses_full")
           or _find_case_file(case_path, "plot_data_06_actinide_masses")
           or _find_case_file(case_path, "actinide_masses"))
    fp = _find_case_file(case_path, "fission_product_masses")
    a_days, a_series = _read_mass_table(act)
    if not a_series:
        return None, None
    f_days, f_series = _read_mass_table(fp)
    have_fp = bool(f_series) and len(f_series) == len(a_series)

    time, comp, tot = a_days, [], []
    for i in range(len(a_series)):
        cm = dict(a_series[i])
        a_kg = sum(a_series[i].values())
        f_kg = 0.0
        if have_fp:
            for nid, kg in f_series[i].items():
                cm[nid] = cm.get(nid, 0.0) + kg
                f_kg += kg
        comp.append(cm)
        tot.append(a_kg + f_kg)

    t = np.asarray(time)
    n = len(t)
    if target_day <= t[0]:
        lo = hi = 0
        fr = 0.0
    elif target_day >= t[-1]:
        lo = hi = n - 1
        fr = 0.0
    else:
        hi = 1
        while hi < n and t[hi] < target_day:
            hi += 1
        lo = hi - 1
        span = t[hi] - t[lo]
        fr = (target_day - t[lo]) / span if span > 0 else 0.0

    out = {}
    for nid, kg in comp[lo].items():
        out[nid] = out.get(nid, 0.0) + (1.0 - fr) * kg
    for nid, kg in comp[hi].items():
        out[nid] = out.get(nid, 0.0) + fr * kg
    mt = (1.0 - fr) * tot[lo] + fr * tot[hi]
    m0 = tot[0]
    mass_ratio = min(1.0, mt / m0) if m0 > 0 else 1.0

    s = sum(out.values())
    if s <= 0:
        return None, None
    return {nid: v / s for nid, v in out.items()}, mass_ratio


# --------------------------------------------------------------------------
# snapshot -> flow
# --------------------------------------------------------------------------
def rise_series(levels):
    """Buffer level at each step -> cumulative kg that ever ENTERED.

    `levels` is a snapshot (kg in the buffer right now), so cumsum() would
    re-count every kg on every step it sits there. Each arrival shows up as a
    positive step change; count those and ignore the drains.

    Lower bound: material that arrives and departs inside one step is never
    seen. Never an over-count, which is the property that matters here.
    """
    levels = np.asarray(levels, dtype=float)
    return np.cumsum(np.maximum(np.diff(levels, prepend=0.0), 0.0))


def buffer_inflow(e, t):
    """Per-nuclide version of rise_series().

    e : DataFrame with (Time, NucId, Quantity) snapshot rows for ONE buffer
    t : full timestep axis to reindex onto
    -> Series NucId -> cumulative kg that ever entered the buffer
    """
    if e.empty:
        return pd.Series(dtype=float)
    p = (e.pivot_table(index="Time", columns="NucId", values="Quantity",
                       aggfunc="sum", fill_value=0.0)
          .reindex(t).fillna(0.0))
    arr = p.values
    rise = np.diff(arr, axis=0, prepend=np.zeros((1, arr.shape[1])))
    return pd.Series(np.maximum(rise, 0.0).sum(axis=0), index=p.columns)


class Db:
    def __init__(self, path):
        if not os.path.exists(path):
            sys.exit("no such file: %s" % path)
        self.conn = sqlite3.connect(path)
        self.tables = {r[0] for r in self.conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        rows = self.conn.execute(
            "SELECT SimId FROM Info ORDER BY rowid DESC LIMIT 1").fetchall()
        self.sim = rows[0][0] if rows else None
        # Cyclus APPENDS to an existing .sqlite. If more than one sim is in
        # here, the newest is plotted and the others are invisible - which is
        # exactly how two different input files produce "identical" figures.
        n = self.conn.execute("SELECT COUNT(*) FROM Info").fetchone()[0]
        if n > 1:
            print("  WARN: %d simulations in this file; plotting the NEWEST "
                  "only.\n        Delete the .sqlite between runs, or you are "
                  "plotting the wrong run." % n)
        self.agents = self.q(
            "SELECT AgentId, Prototype, Spec FROM AgentEntry WHERE SimId=?",
            (self.sim,)) if "AgentEntry" in self.tables else pd.DataFrame()

    def has(self, *names):
        missing = [n for n in names if n not in self.tables]
        if missing:
            print("  skip: missing %s" % ", ".join(missing))
            return False
        return True

    def q(self, sql, params=()):
        return pd.read_sql_query(sql, self.conn, params=params)

    def duration(self):
        d = self.q("SELECT Duration FROM Info WHERE SimId=?", (self.sim,))
        return int(d.Duration.iloc[0]) if not d.empty else 0

    def aid(self, prototype):
        if self.agents.empty:
            return None
        m = self.agents[self.agents.Prototype == prototype]
        return int(m.AgentId.iloc[0]) if not m.empty else None

    def aid_by_spec(self, frag):
        if self.agents.empty:
            return None
        m = self.agents[self.agents.Spec.str.contains(frag, na=False)]
        return int(m.AgentId.iloc[0]) if not m.empty else None

    def einv(self, agent=None, buf=None):
        sql = ("SELECT Time, AgentId, InventoryName, NucId, Quantity "
               "FROM ExplicitInventory WHERE SimId=?")
        p = [self.sim]
        if agent is not None:
            sql += " AND AgentId=?"
            p.append(agent)
        if buf is not None:
            sql += " AND InventoryName=?"
            p.append(buf)
        return self.q(sql, tuple(p))

    def total_charged(self):
        """SUM(AdsEvents.Discharge) = total mass ever loaded into the core.
        An UPPER bound on anything the ADS can ever have discharged."""
        if "AdsEvents" not in self.tables:
            return 0.0
        v = self.q("SELECT SUM(Value) AS v FROM AdsEvents WHERE SimId=? AND "
                   "Event='Discharge'", (self.sim,))
        return float(v.v.iloc[0] or 0.0)

    def discharge_summary(self):
        """(matched_case, mean m_in per discharge, n_discharges) or None."""
        if "AdsEvents" not in self.tables:
            return None
        d = self.q("SELECT MatchedCase, Value FROM AdsEvents WHERE SimId=? AND "
                   "Event='Discharge'", (self.sim,))
        if d.empty:
            return None
        m = d.MatchedCase.mode()
        case = m.iloc[0] if not m.empty else d.MatchedCase.iloc[0]
        return case, float(d.Value.mean()), int(len(d))


def groups(df):
    """(Time, NucId, Quantity) -> per-Time ma/pu/u/othact/fp/act/tot."""
    if df.empty:
        return pd.DataFrame(columns=["Time", "ma", "pu", "u", "othact", "fp",
                                     "act", "tot"])
    d = df.copy()
    z = d.NucId // 10000000
    d["grp"] = np.where(z.isin(MA_Z), "ma",
                np.where(z == PU_Z, "pu",
                np.where(z == U_Z, "u",
                np.where(z >= ACT_Z_MIN, "othact", "fp"))))
    p = d.pivot_table(index="Time", columns="grp", values="Quantity",
                      aggfunc="sum", fill_value=0.0).reset_index()
    for c in ("ma", "pu", "u", "othact", "fp"):
        if c not in p:
            p[c] = 0.0
    p["act"] = p.ma + p.pu + p.u + p.othact
    p["tot"] = p.act + p.fp
    return p


def save(fig, outdir, name):
    path = os.path.join(outdir, name)
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)
    print("  wrote %s" % path)


# --------------------------------------------------------------------------
def fig_ma(db, outdir, ads, sink=None):
    print("fig 1: MA loaded / discharged / destroyed")
    if not db.has("ExplicitInventory", "AgentEntry"):
        return
    a = db.aid(ads) or db.aid_by_spec("ads")
    if a is None:
        print("  skip: no ADS agent named '%s'" % ads)
        return
    gc = groups(db.einv(a, "core"))
    if gc.empty or gc.ma.max() <= 0:
        print("  skip: `core` never held MA - the ADS never loaded")
        return

    tmax = db.duration() or int(gc.Time.max()) + 1
    t = np.arange(tmax + 1)
    core_ma = pd.Series(gc.set_index("Time").ma).reindex(t).fillna(0.0).values
    core_pu = pd.Series(gc.set_index("Time").pu).reindex(t).fillna(0.0).values

    # a load = the start of a contiguous run of steps with MA in the core.
    # This needs an idle step between cycles to separate them; refuel_time > 0
    # guarantees that. The cross-check against AdsEvents below catches it if
    # refuel_time is ever set to 0 and two cycles merge into one "load".
    on = core_ma > 1e-9
    edges = np.diff(np.concatenate(([0], on.astype(int), [0])))
    starts = np.where(edges == 1)[0]
    cum_in = np.zeros(tmax + 1)
    for s in starts:
        cum_in[s:] += core_ma[s]

    if "AdsEvents" in db.tables:
        nl = db.q("SELECT COUNT(*) AS n FROM AdsEvents WHERE SimId=? AND "
                  "Event='Load'", (db.sim,))
        nl = int(nl.n.iloc[0] or 0)
        if nl and nl != len(starts):
            print("  WARN: counted %d loads from `core`, but AdsEvents has %d."
                  "\n        Cycles are merging - is refuel_time 0?"
                  % (len(starts), nl))

    # ---- MA out: sink first, then `product`, then give up ------------------
    cum_out, src = None, None
    if sink:
        sa = db.aid(sink)
        if sa is None:
            print("  warn: no agent named '%s'" % sink)
        else:
            gs = groups(db.einv(sa))
            if not gs.empty and gs.ma.max() > 0:
                # A Sink never gives material back, so its inventory really is
                # cumulative; ffill just spans gaps in the snapshot record.
                cum_out = (pd.Series(gs.set_index("Time").ma)
                           .reindex(t).ffill().fillna(0.0).values)
                src = "%s inventory (cumulative)" % sink
    if cum_out is None:
        gp = groups(db.einv(a, "product"))
        if not gp.empty and gp.ma.max() > 0:
            # FIX: `product` is a SNAPSHOT. The old code did .cumsum() on it,
            # re-counting every kg on every step it sat in the buffer, which
            # produced ~90x inflation and a negative "MA destroyed". Count the
            # positive increments instead - one per discharge.
            prod_ma = (pd.Series(gp.set_index("Time").ma)
                       .reindex(t).fillna(0.0).values)
            cum_out = rise_series(prod_ma)
            src = "ADS `product` buffer (positive increments)"

    # ---- sanity: MA out cannot exceed everything ever charged --------------
    cap = db.total_charged()
    if cum_out is not None and cap > 0 and cum_out[-1] > cap * 1.001:
        print("  WARN: MA out (%.0f kg) exceeds total mass ever charged "
              "(%.0f kg)." % (cum_out[-1], cap))
        print("        The MA-out source is not measuring a flow. Do not "
              "trust this panel.")

    fig, ax = plt.subplots(2, 1, figsize=(11, 7.5), sharex=True)
    ax[0].plot(t, core_ma, lw=1.6, color=ORANGE, label="MA in core")
    if core_pu.max() > 0:
        ax[0].plot(t, core_pu, lw=1.6, color=BLUE, label="Pu in core")
    ax[0].set_ylabel("kg in core")
    ax[0].set_title("%s core loading - %d loads (flat while irradiating: the "
                    "archetype transmutes only at discharge)"
                    % (ads, len(starts)), fontsize=10)
    ax[0].legend()
    ax[0].grid(alpha=.3)

    ax[1].plot(t, cum_in, lw=2, color=ORANGE, label="cumulative MA loaded")
    if cum_out is not None:
        net = cum_in - cum_out
        ax[1].plot(t, cum_out, lw=2, color=BLUE,
                   label="cumulative MA discharged")
        ax[1].plot(t, net, lw=2.4, color=DARK, label="cumulative MA DESTROYED")
        ax[1].fill_between(t, 0, net, color=DARK, alpha=.12)
        ax[1].annotate("%.0f kg MA destroyed\n(%.1f%% of %.0f kg loaded)"
                       % (net[-1], 100 * net[-1] / max(cum_in[-1], 1e-9),
                          cum_in[-1]),
                       xy=(t[-1], net[-1]), xytext=(-195, 28),
                       textcoords="offset points",
                       arrowprops=dict(arrowstyle="->"))
        print("     -> MA out source: %s" % src)
        print("     -> %.1f kg loaded, %.1f kg discharged, %.1f kg DESTROYED"
              % (cum_in[-1], cum_out[-1], net[-1]))
    else:
        ax[1].text(.5, .45,
                   "no MA-out source found\n\n"
                   "once-through runs: pass --sink SpentFuelStore\n"
                   "recycle runs: `product` is traded away before the\n"
                   "inventory snapshot, so use fig 2 / fig 12 instead",
                   ha="center", va="center", transform=ax[1].transAxes,
                   color=RED, fontsize=9)
        print("     -> no MA-out source; only the loaded curve is drawn")
    ax[1].set_ylabel("cumulative kg")
    ax[1].set_xlabel("timestep [months]")
    ax[1].legend(fontsize=9, loc="upper left")
    ax[1].grid(alpha=.3)
    save(fig, outdir, "01_ma_destroyed.png")


def fig_mass_balance(db, outdir):
    """MassDefect must be ~0.09% of ActinideDestroyed. 100% = the
    fission-product table is not loading.

    From ads.cc: MassDefect = m_in - m_out, and (by construction)
    ActinideDestroyed - FissionProducts = MassDefect exactly. So this ratio
    is really asking a question about the SERPENT TABLES, not about the
    archetype: how much mass do the actinide + FP tables lose between day 0
    and day d? Physics says ~0.09%. Anything above that is mass the two
    tables do not account for.
    """
    print("fig 2: mass balance")
    if not db.has("AdsEvents"):
        print("  -> the ADS never loaded a core")
        return
    ev = db.q("SELECT Time, Event, Value FROM AdsEvents WHERE SimId=?",
              (db.sim,))
    if ev.empty:
        return
    p = ev.pivot_table(index="Time", columns="Event", values="Value",
                       aggfunc="sum", fill_value=0.0)
    for c in ("ActinideDestroyed", "FissionProducts", "MassDefect"):
        if c not in p:
            p[c] = 0.0
    d = p[p.ActinideDestroyed > 0]
    if d.empty:
        print("  skip: no discharges yet")
        return

    fig, ax = plt.subplots(2, 1, figsize=(11, 7))
    x = np.arange(len(d.index))
    w = .28
    ax[0].bar(x - w, d.ActinideDestroyed, w, label="ActinideDestroyed",
              color=BLUE)
    ax[0].bar(x, d.FissionProducts, w, label="FissionProducts", color=ORANGE)
    ax[0].bar(x + w, d.MassDefect, w, label="MassDefect", color=DARK)
    ax[0].set_xticks(x)
    ax[0].set_xticklabels(d.index, rotation=90, fontsize=7)
    ax[0].set_ylabel("kg")
    ax[0].set_title("Per-discharge mass accounting")
    ax[0].legend()
    ax[0].grid(alpha=.3, axis="y")

    ratio = 100.0 * d.MassDefect / d.ActinideDestroyed
    ax[1].plot(x, ratio, "o-", color=RED)
    ax[1].axhline(0.09, ls="--", c="k",
                  label="physical ~0.09%  (200 MeV / 240 u)")
    ax[1].axhline(100.0, ls=":", c=RED,
                  label="100% = fission products NOT loaded")
    ax[1].set_yscale("symlog", linthresh=.05)
    ax[1].set_ylabel("MassDefect / ActinideDestroyed [%]")
    ax[1].set_xlabel("discharge")
    ax[1].set_xticks(x)
    ax[1].set_xticklabels(d.index, rotation=90, fontsize=7)
    ax[1].legend(fontsize=8)
    ax[1].grid(alpha=.3)
    med = np.median(ratio)
    print("     -> median MassDefect/ActinideDestroyed = %.3f %%  (%s)"
          % (med, "OK" if med < 5 else "FP TABLE NOT LOADING"))
    if 0.2 < med < 5:
        print("        (%.2fx the physical 0.09%%: the Serpent actinide + FP"
              % (med / 0.0894))
        print("         tables lose ~%.2f%% of mass that neither one holds.)"
              % (med - 0.0894))
    save(fig, outdir, "02_mass_balance.png")


def fig_duty(db, outdir):
    print("fig 3: duty cycle")
    if not db.has("AdsEvents"):
        return
    ev = db.q("SELECT Time, Event FROM AdsEvents WHERE SimId=? AND "
              "Event IN ('Load','Discharge') ORDER BY Time", (db.sim,))
    if ev.empty:
        return
    tmax = db.duration() or int(ev.Time.max()) + 1
    on, loaded = False, np.zeros(tmax + 1)
    for t in range(tmax + 1):
        r = ev[ev.Time == t]
        if (r.Event == "Discharge").any():
            on = False
        if (r.Event == "Load").any():
            on = True
        loaded[t] = 1 if on else 0

    fig, ax = plt.subplots(figsize=(11, 3.2))
    ax.fill_between(range(tmax + 1), 0, loaded, step="post", color=BLUE,
                    alpha=.6)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["idle", "irradiating"])
    ax.set_xlabel("timestep [months]")
    ax.set_title("ADS core state - %d loads, %d discharges, duty %.0f%%"
                 % (int((ev.Event == "Load").sum()),
                    int((ev.Event == "Discharge").sum()), 100 * loaded.mean()))
    ax.grid(alpha=.3, axis="x")
    save(fig, outdir, "03_duty_cycle.png")


def fig_charge_comp(db, outdir, ads, ref=None):
    """Is the charge what the Serpent case assumed?
       --ref 50.19 49.81   Case 4 (50/50 Pu/MA)
       --ref 0 100         Case 3 (MA-only)

    FIX: the denominator is now ACTINIDE mass, not total mass. ads.cc
    fingerprints each case with in_pu_frac = pu / act (actinides only, Z>=89),
    and MatchCase_ compares against that. The old code divided by tot =
    act + fp, so any recycled fission-product carryover diluted the plotted
    percentages relative to the reference lines and made a matching charge
    look like a mismatched one.
    """
    print("fig 4: charge composition")
    if not db.has("ExplicitInventory", "AgentEntry"):
        return
    a = db.aid(ads) or db.aid_by_spec("ads")
    if a is None:
        return
    g = groups(db.einv(a, "core"))
    g = g[g.act > 1e-9]
    if g.empty:
        print("  skip: core never loaded")
        return

    fig, ax = plt.subplots(figsize=(11, 4.2))
    ax.plot(g.Time, 100 * g.pu / g.act, ".", ms=4, color=BLUE, label="Pu")
    ax.plot(g.Time, 100 * g.ma / g.act, ".", ms=4, color=ORANGE, label="MA")
    if ref:
        ax.axhline(ref[0], ls="--", c=BLUE, alpha=.7,
                   label="Serpent charge: %.2f%% Pu" % ref[0])
        ax.axhline(ref[1], ls="--", c=ORANGE, alpha=.7,
                   label="Serpent charge: %.2f%% MA" % ref[1])
        dpu = float((100 * g.pu / g.act).mean()) - ref[0]
        dma = float((100 * g.ma / g.act).mean()) - ref[1]
        print("     -> mean charge vs Serpent: Pu %+.2f pp, MA %+.2f pp"
              % (dpu, dma))
    else:
        print("     -> no --ref given; no reference lines drawn")
    # How much non-actinide is riding along? Should be ~0 for a fresh charge.
    nonact = float((100 * g.fp / g.tot).mean())
    if nonact > 0.5:
        print("     -> NOTE %.1f%% of the charge is non-actinide (Z<89); the "
              "wt%% here are on an actinide basis, matching Serpent." % nonact)
    ax.set_ylabel("wt% of ACTINIDE in charge")
    ax.set_xlabel("timestep [months]")
    ax.set_ylim(-3, 103)
    ax.set_title("ADS charge vs the Serpent case it is matched to "
                 "(actinide basis, as in MatchCase_)", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=.3)
    save(fig, outdir, "04_charge_composition.png")


def fig_flows(db, outdir):
    print("fig 5: commodity flows")
    ts = sorted(tb for tb in db.tables if tb.startswith("TimeSeriessupply")
                or tb.startswith("TimeSeriesdemand"))
    if not ts:
        print("  skip: no TimeSeries* tables")
        return
    fig, ax = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    any_data = False
    for tb in ts:
        df = db.q("SELECT Time, SUM(Value) AS v FROM %s WHERE SimId=? "
                  "GROUP BY Time" % tb, (db.sim,))
        if df.empty:
            continue
        any_data = True
        lab = tb.replace("TimeSeries", "")
        ax[0].plot(df.Time, df.v, lw=1, label=lab)
        ax[1].plot(df.Time, df.v.cumsum(), lw=1, label=lab)
    if not any_data:
        plt.close(fig)
        return
    for axis, yl in ((ax[0], "kg / step (log)"),
                     (ax[1], "cumulative kg (log)")):
        axis.set_yscale("log")
        axis.set_ylabel(yl)
        axis.grid(alpha=.3)
        axis.legend(fontsize=7, ncol=3)
    ax[0].set_title("Commodity supply / demand")
    ax[1].set_xlabel("timestep [months]")
    save(fig, outdir, "05_commodity_flows.png")


def fig_buffers(db, outdir):
    """Flat top = the cap is binding = backpressure is reaching upstream."""
    print("fig 6: buffer inventories")
    if not db.has("ExplicitInventory", "AgentEntry"):
        return
    df = db.q("""SELECT Time, AgentId, InventoryName AS Buf,
                        SUM(Quantity) AS kg
                 FROM ExplicitInventory WHERE SimId=?
                 GROUP BY Time, AgentId, InventoryName""", (db.sim,))
    if df.empty:
        return
    df = df.merge(db.agents[["AgentId", "Prototype"]], on="AgentId", how="left")
    df["key"] = df.Prototype.fillna("agent?") + " / " + df.Buf
    p = df.pivot_table(index="Time", columns="key", values="kg",
                       aggfunc="sum", fill_value=0.0)

    fig, ax = plt.subplots(figsize=(11, 5.5))
    for c in p.columns:
        if p[c].max() > 0:
            ax.plot(p.index, p[c], lw=1.2, label=c)
    ax.set_yscale("log")
    ax.set_ylabel("kg (log)")
    ax.set_xlabel("timestep [months]")
    ax.set_title("Buffer inventories - a flat top means the cap is binding")
    ax.legend(fontsize=7, ncol=2)
    ax.grid(alpha=.3)
    save(fig, outdir, "06_buffers.png")


def fig_snf(db, outdir):
    print("fig 7: SNF drawdown")
    if not db.has("us_inventorySupply"):
        return
    df = db.q("SELECT Time, Quantity, AssemblyId FROM us_inventorySupply "
              "WHERE SimId=?", (db.sim,))
    if df.empty:
        return
    g = df.groupby("Time").agg(kg=("Quantity", "sum"),
                               n=("AssemblyId", "nunique")).reset_index()
    fig, ax = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    ax[0].plot(g.Time, g.kg, color=BLUE)
    ax[0].set_ylabel("SNF supplied [kg/step]")
    ax[0].set_title("us_inventory supply - %.0f t total, %d assemblies"
                    % (df.Quantity.sum() / 1e3, df.AssemblyId.nunique()))
    ax[0].grid(alpha=.3)
    a2 = ax[0].twinx()
    a2.plot(g.Time, g.n, color=ORANGE, ls="--")
    a2.set_ylabel("assemblies / step", color=ORANGE)
    ax[1].plot(g.Time, g.kg.cumsum() / 1e3, color=DARK, lw=2)
    ax[1].set_ylabel("cumulative SNF drawn [t]")
    ax[1].set_xlabel("timestep [months]")
    ax[1].grid(alpha=.3)
    # This is the number that should DIFFER between recycle and once-through.
    print("     -> %.1f t fresh SNF drawn over the run" % (df.Quantity.sum() / 1e3))
    save(fig, outdir, "07_snf_drawdown.png")


def fig_policy(db, outdir):
    """MA wt% of the SNF waiting in the Separations feed buffer.
    Sloping down => the selection policy is really ordering the inventory.
    Flat => the policy is inert (buffers unbounded, everything separated)."""
    print("fig 8: selection policy")
    if not db.has("ExplicitInventory", "AgentEntry"):
        return
    a = db.aid_by_spec("Separations")
    if a is None:
        print("  skip: no Separations agent")
        return
    e = db.einv(a)
    if e.empty:
        return
    bufs = [b for b in e.InventoryName.unique() if "feed" in b.lower()]
    if not bufs:
        print("  skip: no feed-like buffer on Separations")
        return
    if len(bufs) > 1:
        print("     -> %d feed-like buffers %s; using '%s'"
              % (len(bufs), bufs, bufs[0]))
    g = groups(e[e.InventoryName == bufs[0]])
    g = g[g.tot > 1e-9]
    if g.empty:
        return
    fig, ax = plt.subplots(figsize=(11, 4.2))
    ax.plot(g.Time, 1e2 * g.ma / g.tot, color=ORANGE)
    ax.set_ylabel("MA content of SNF awaiting separation [wt%]")
    ax.set_xlabel("timestep [months]")
    ax.set_title("Selection policy check on '%s' - downward slope = the "
                 "policy bites; flat = inert" % bufs[0], fontsize=10)
    ax.grid(alpha=.3)
    save(fig, outdir, "08_policy_check.png")


def fig_map(db, outdir):
    print("fig 9: provenance map")
    if not db.has("us_inventorySupply"):
        return
    df = db.q("""SELECT Facility, Latitude, Longitude, SUM(Quantity) AS kg,
                        COUNT(*) AS n FROM us_inventorySupply WHERE SimId=?
                 GROUP BY Facility, Latitude, Longitude""", (db.sim,))
    df = df[(df.Latitude != 0) | (df.Longitude != 0)]
    if df.empty:
        print("  skip: no coordinates")
        return
    fig, ax = plt.subplots(figsize=(11, 6))
    s = ax.scatter(df.Longitude, df.Latitude,
                   s=25 + 300 * df.kg / df.kg.max(), c=df.kg / 1e3,
                   cmap="plasma", alpha=.85, edgecolor="k", lw=.4)
    fig.colorbar(s, ax=ax, label="SNF drawn [t]")
    ax.set_xlabel("longitude")
    ax.set_ylabel("latitude")
    ax.set_title("Where the SNF came from (%d facilities)" % len(df))
    ax.grid(alpha=.3)
    save(fig, outdir, "09_provenance_map.png")


def fig_discharge(db, outdir, ads, case_dir, days_per_step=None,
                  cycle_time=None):
    """Composition discharged from the ADS core, BEFORE separations/sink.

    Reconstructed from the matched Serpent case (see reconstruct_discharge):
    the discharge is traded away in-step and this build has no Resources table,
    so the composition cannot be read from the database. We read the case named
    in AdsEvents.MatchedCase, interpolate to the irradiation day, scale by the
    loaded mass and the number of discharges, and plot the top-25 nuclides
    grouped by element (U / Pu / MA / other actinide / fission product).

    Comment #4 fix: the OLD version read the whole waste Sink, which in the
    ADS 1 flowsheet also holds the Separations reject stream (U, Pu, FP) that
    never entered the core - the uranium the second reader asked about. This
    shows ONLY what the core discharges.
    """
    print("fig 10: ADS discharge composition (from matched Serpent case)")
    summ = db.discharge_summary()
    if summ is None:
        print("  skip: no Discharge events in AdsEvents")
        return
    case_name, m_in, n_dis = summ
    if not case_name:
        print("  skip: MatchedCase is blank on the Discharge rows")
        return
    if not case_dir:
        print("  skip: pass --case-dir /path/to/serpent_cases to build fig 10")
        return
    if cycle_time is None:
        print("  skip: pass --cycle-time (steps) to set the irradiation day")
        return
    if days_per_step is None:
        days_per_step = 30.4375
        print("     -> --days-per-step not given; assuming %.4f d/step"
              % days_per_step)

    case_path = os.path.join(case_dir, case_name)
    if not os.path.isdir(case_path):
        # tolerate case_dir already pointing straight at the single case folder
        if _find_case_file(case_dir, "actinide_masses"):
            case_path = case_dir
        else:
            print("  skip: case folder not found: %s" % case_path)
            return

    target_day = cycle_time * days_per_step
    frac, mass_ratio = reconstruct_discharge(case_path, target_day)
    if frac is None:
        print("  skip: could not read case tables in %s" % case_path)
        return

    per_cycle_kg = m_in * mass_ratio
    d = pd.Series({nid: f * per_cycle_kg * n_dis for nid, f in frac.items()})
    d = d[d > 0].sort_values(ascending=False)
    if d.empty:
        print("  skip: empty reconstructed composition")
        return

    bycat = {}
    for nid, kg in d.items():
        bycat[category(nid)] = bycat.get(category(nid), 0.0) + kg
    tot = d.sum()
    print("     -> case '%s', target_day=%.1f, mass_ratio=%.4f"
          % (case_name, target_day, mass_ratio))
    print("     -> cumulative discharge over %d cycles: %.1f kg" % (n_dis, tot))
    for c in ("MA", "Pu", "U", "OtherAct", "FP"):
        if bycat.get(c, 0) > 0:
            print("        %-8s %10.1f kg  (%.2f%%)"
                  % (c, bycat[c], 100 * bycat[c] / tot))

    top = d.head(25)
    labels = [nucname(n) for n in top.index]
    colors = [CATCOLOR[category(n)] for n in top.index]
    fig, ax = plt.subplots(figsize=(11, 5.5))
    ax.bar(labels, top.values, color=colors)
    ax.set_yscale("log")
    ax.set_ylabel("kg (log)")
    ax.set_title("Material discharged from %s core (matched case %s), "
                 "before separation - top 25 nuclides" % (ads, case_name),
                 fontsize=9)
    ax.tick_params(axis="x", rotation=90)
    ax.grid(alpha=.3, axis="y")
    present = [c for c in ("MA", "Pu", "U", "OtherAct", "FP")
               if any(category(n) == c for n in top.index)]
    ax.legend(handles=[Patch(color=CATCOLOR[c], label=c) for c in present],
              fontsize=8)
    save(fig, outdir, "10_discharge_composition.png")


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  Case 4 recycle   : plots.py db.sqlite --ref 50.19 49.81\n"
               "  Case 3 recycle   : plots.py db.sqlite --ref 0 100\n"
               "  Case 3 once-thru : plots.py db.sqlite --ref 0 100 "
               "--sink SpentFuelStore\n")
    ap.add_argument("db")
    ap.add_argument("-o", "--outdir", default="figs")
    ap.add_argument("--ads", default="MyADS",
                    help="ADS prototype name (default MyADS)")
    ap.add_argument("--ref", nargs=2, type=float, metavar=("PU_PCT", "MA_PCT"),
                    help="Serpent charge split for the fig-4 reference lines, "
                         "on an ACTINIDE basis: '--ref 50.19 49.81' for "
                         "Case 4, '--ref 0 100' for Case 3")
    ap.add_argument("--sink", default=None,
                    help="once-through store prototype (e.g. SpentFuelStore). "
                         "Used by fig 1 for the cumulative MA-out curve.")
    ap.add_argument("--case-dir", default=None,
                    help="path to the Serpent case library (the archetype's "
                         "case_dir), with one folder per case. Needed for "
                         "fig 10.")
    ap.add_argument("--cycle-time", type=int, default=None,
                    help="ADS cycle length in time steps (120 for ADS 1, "
                         "24 for ADS 2). Sets the irradiation day for fig 10.")
    ap.add_argument("--days-per-step", type=float, default=None,
                    help="days per Cyclus time step (default 30.4375 if unset)")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    db = Db(a.db)
    print("tables: %d   agents: %d\n" % (len(db.tables), len(db.agents)))
    for _, r in db.agents.iterrows():
        print("   %3d  %-18s %s" % (r.AgentId, r.Prototype, r.Spec))
    print()
    fig_ma(db, a.outdir, a.ads, a.sink)
    fig_mass_balance(db, a.outdir)
    fig_duty(db, a.outdir)
    fig_charge_comp(db, a.outdir, a.ads, a.ref)
    fig_flows(db, a.outdir)
    fig_buffers(db, a.outdir)
    fig_snf(db, a.outdir)
    fig_policy(db, a.outdir)
    fig_map(db, a.outdir)
    fig_discharge(db, a.outdir, a.ads, a.case_dir, a.days_per_step,
                  a.cycle_time)
    print("\ndone -> %s/" % a.outdir)


if __name__ == "__main__":
    main()