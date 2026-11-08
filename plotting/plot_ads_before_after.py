#!/usr/bin/env python3
"""
plot_issue5_before_after.py

Create the before-and-after stacked bar figure requested in thesis review
Comment #5:

    "In both Figures 4.11 and 4.12, it would be more informative to show
    a stacked bar with the before and after values instead of just the end
    discharge amounts."

The script reads ONE matched ADS irradiation cycle from a Cyclus SQLite
database:

  1. Core input:
     the ADS `core` inventory at the latest Load event before the chosen
     discharge.

  2. ADS discharge:
     the clean ADS-only output recovered at the downstream receiver on the
     discharge timestep. This avoids the front-end separations reject stream.

The nuclides are aggregated into:
    Np, Am, Cm, Pu, U, Other actinides, and FP + other non-actinides.

Requirements
------------
The Cyclus input must have:
    <explicit_inventory>1</explicit_inventory>

Required tables:
    Info, AgentEntry, AdsEvents, Transactions, ExplicitInventory

Examples
--------
ADS 1:
    python3 plot_issue5_before_after.py ma_recycle_100.sqlite \
        --case-label "ADS 1" \
        --output ads1_before_after.png

ADS 2:
    python3 plot_issue5_before_after.py puma_recycle_100.sqlite \
        --case-label "ADS 2" \
        --output ads2_before_after.png

Optional:
    --ads MyADS
        ADS prototype name. Default: MyADS

    --csv ads1_before_after.csv
        Save the plotted values to a CSV file.

Notes
-----
* AdsEvents.Discharge stores the mass entering the ADS transformation.
* The expected output mass is:
      Discharge - MassDefect
* ExplicitInventory is a snapshot, not a cumulative flow. This script uses
  one clean matched snapshot; it never sums inventory snapshots over time.
"""

import argparse
import os
import re
import sqlite3
import sys
from typing import Optional, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


ACTINIDE_Z_MIN = 89

GROUP_ORDER = [
    "Np",
    "Am",
    "Cm",
    "Pu",
    "U",
    "Other actinides",
    "FP + other non-actinides",
]


class CyclusDatabase:
    """Small helper for reading the newest simulation in a Cyclus database."""

    def __init__(self, path: str):
        if not os.path.isfile(path):
            raise FileNotFoundError(f"No such database: {path}")

        self.path = path
        self.conn = sqlite3.connect(path)

        self.tables = {
            row[0]
            for row in self.conn.execute(
                "SELECT name FROM sqlite_master WHERE type='table'"
            )
        }

        required = {
            "Info",
            "AgentEntry",
            "AdsEvents",
            "Transactions",
            "ExplicitInventory",
        }
        missing = sorted(required - self.tables)
        if missing:
            raise RuntimeError(
                "Database is missing required table(s): " + ", ".join(missing)
            )

        sims = self.conn.execute(
            "SELECT SimId FROM Info ORDER BY rowid DESC"
        ).fetchall()

        if not sims:
            raise RuntimeError("The Info table contains no simulations.")

        self.sim_id = sims[0][0]

        if len(sims) > 1:
            print(
                f"WARNING: {len(sims)} simulations are stored in this file; "
                "using the newest simulation only."
            )

        self.agents = self.query(
            """SELECT AgentId, Prototype, Spec
               FROM AgentEntry
               WHERE SimId=?""",
            (self.sim_id,),
        )

    def query(self, sql: str, params=()) -> pd.DataFrame:
        return pd.read_sql_query(sql, self.conn, params=params)

    def close(self) -> None:
        self.conn.close()

    def find_ads(self, prototype: str) -> int:
        exact = self.agents[self.agents["Prototype"] == prototype]

        if not exact.empty:
            return int(exact.iloc[0]["AgentId"])

        by_spec = self.agents[
            self.agents["Spec"].str.contains("ads", case=False, na=False)
        ]

        if len(by_spec) == 1:
            agent_id = int(by_spec.iloc[0]["AgentId"])
            found_name = by_spec.iloc[0]["Prototype"]
            print(
                f"ADS prototype '{prototype}' was not found; "
                f"using '{found_name}' (AgentId {agent_id})."
            )
            return agent_id

        if by_spec.empty:
            raise RuntimeError(
                f"No ADS agent named '{prototype}' and no unique ADS spec "
                "were found."
            )

        candidates = ", ".join(
            f"{row.Prototype} (AgentId {row.AgentId})"
            for row in by_spec.itertuples(index=False)
        )
        raise RuntimeError(
            f"ADS prototype '{prototype}' was not found and multiple ADS "
            f"agents exist: {candidates}. Pass --ads with the desired "
            "prototype name."
        )


def nuclide_group(nucid: int) -> str:
    """Return the plotting group for a Cyclus ZZAAASSSS nuclide ID."""

    z = int(nucid) // 10_000_000

    if z == 93:
        return "Np"
    if z == 95:
        return "Am"
    if z == 96:
        return "Cm"
    if z == 94:
        return "Pu"
    if z == 92:
        return "U"
    if z >= ACTINIDE_Z_MIN:
        return "Other actinides"
    return "FP + other non-actinides"


def aggregate_groups(composition: pd.Series) -> pd.Series:
    """Aggregate a NucId-indexed mass composition into plotting groups."""

    totals = pd.Series(0.0, index=GROUP_ORDER, dtype=float)

    for nucid, mass_kg in composition.items():
        if mass_kg <= 0.0:
            continue
        totals[nuclide_group(int(nucid))] += float(mass_kg)

    return totals


def ads_events_by_time(
    db: CyclusDatabase, ads_id: int
) -> pd.DataFrame:
    """Return AdsEvents in a Time x Event table."""

    events = db.query(
        """SELECT Time, Event, SUM(Value) AS Value
           FROM AdsEvents
           WHERE SimId=? AND AgentId=?
           GROUP BY Time, Event
           ORDER BY Time""",
        (db.sim_id, ads_id),
    )

    if events.empty:
        raise RuntimeError("The selected ADS has no AdsEvents records.")

    pivot = events.pivot_table(
        index="Time",
        columns="Event",
        values="Value",
        aggfunc="sum",
        fill_value=0.0,
    )

    for column in ("Load", "Discharge", "MassDefect"):
        if column not in pivot.columns:
            pivot[column] = 0.0

    return pivot


def clean_ads_discharge(
    db: CyclusDatabase, ads_id: int, events: pd.DataFrame
) -> Tuple[pd.Series, int, int, str, float]:
    """Recover the latest clean ADS-only discharge composition.

    The ADS `product` buffer may be emptied by a trade before
    ExplicitInventory is written. Therefore, this function follows the ADS
    transaction to its receiver and looks for a receiver buffer whose total
    mass matches the expected ADS output.

    To avoid mixing the ADS output with fresh spent fuel or another stream,
    the selected receiver must have exactly one incoming transaction during
    that timestep.

    Returns
    -------
    composition
        Series indexed by NucId, values in kg.
    discharge_time
        Timestep of the selected ADS discharge.
    receiver_id
        Receiving agent ID.
    buffer_name
        Receiver inventory containing the clean output.
    expected_output_mass
        AdsEvents.Discharge - AdsEvents.MassDefect, in kg.
    """

    transactions = db.query(
        """SELECT TransactionId, ReceiverId, Commodity, Time
           FROM Transactions
           WHERE SimId=? AND SenderId=?
           ORDER BY Time DESC, TransactionId DESC""",
        (db.sim_id, ads_id),
    )

    if transactions.empty:
        raise RuntimeError("The selected ADS made no outgoing transactions.")

    diagnostic_messages = []

    # Search newest to oldest so the selected cycle is normally the last
    # completed cycle in the simulation.
    for tx in transactions.itertuples(index=False):
        time = int(tx.Time)
        receiver_id = int(tx.ReceiverId)

        if time not in events.index or events.loc[time, "Discharge"] <= 0.0:
            diagnostic_messages.append(
                f"time {time}: no matching AdsEvents Discharge"
            )
            continue

        incoming = db.query(
            """SELECT TransactionId, SenderId, Commodity
               FROM Transactions
               WHERE SimId=? AND ReceiverId=? AND Time=?""",
            (db.sim_id, receiver_id, time),
        )

        if len(incoming) != 1:
            diagnostic_messages.append(
                f"time {time}: receiver had {len(incoming)} incoming "
                "transactions"
            )
            continue

        discharge_input_mass = float(events.loc[time, "Discharge"])
        mass_defect = float(events.loc[time, "MassDefect"])
        expected_output_mass = discharge_input_mass - mass_defect

        snapshot = db.query(
            """SELECT InventoryName, NucId, SUM(Quantity) AS Quantity
               FROM ExplicitInventory
               WHERE SimId=? AND AgentId=? AND Time=?
               GROUP BY InventoryName, NucId""",
            (db.sim_id, receiver_id, time),
        )

        if snapshot.empty:
            diagnostic_messages.append(
                f"time {time}: receiver had no ExplicitInventory snapshot"
            )
            continue

        buffer_totals = snapshot.groupby("InventoryName")["Quantity"].sum()

        # Relative tolerance permits tiny floating-point differences while
        # remaining much smaller than a physically meaningful material flow.
        tolerance = max(1.0e-6 * expected_output_mass, 1.0e-3)

        matching = buffer_totals[
            np.abs(buffer_totals - expected_output_mass) <= tolerance
        ]

        if matching.empty:
            diagnostic_messages.append(
                f"time {time}: no receiver buffer matched expected output "
                f"{expected_output_mass:.6f} kg"
            )
            continue

        # If more than one buffer somehow matches, choose the closest total.
        differences = np.abs(matching - expected_output_mass)
        buffer_name = str(differences.idxmin())

        selected = snapshot[
            snapshot["InventoryName"] == buffer_name
        ]

        composition = (
            selected.groupby("NucId")["Quantity"]
            .sum()
            .astype(float)
            .sort_index()
        )
        composition = composition[composition > 0.0]

        actual_output_mass = float(composition.sum())

        if abs(actual_output_mass - expected_output_mass) > tolerance:
            diagnostic_messages.append(
                f"time {time}: selected composition total "
                f"{actual_output_mass:.6f} kg did not match expected output"
            )
            continue

        return (
            composition,
            time,
            receiver_id,
            buffer_name,
            expected_output_mass,
        )

    details = "\n  ".join(diagnostic_messages[:12])
    raise RuntimeError(
        "No clean ADS-only discharge snapshot was found.\n"
        "The search rejected candidate timesteps for these reasons:\n  "
        + details
    )


def matching_core_input(
    db: CyclusDatabase,
    ads_id: int,
    events: pd.DataFrame,
    discharge_time: int,
) -> Tuple[pd.Series, int, int]:
    """Recover the core composition loaded for the selected discharge."""

    load_times = events.index[
        (events.index < discharge_time) & (events["Load"] > 0.0)
    ]

    if len(load_times) == 0:
        raise RuntimeError(
            f"No ADS Load event occurs before discharge time {discharge_time}."
        )

    load_time = int(load_times.max())

    # Prefer the exact load timestep. If Cyclus writes the core snapshot one
    # step later in another version/schema, search forward within the cycle.
    available = db.query(
        """SELECT DISTINCT Time
           FROM ExplicitInventory
           WHERE SimId=?
             AND AgentId=?
             AND InventoryName='core'
             AND Time>=?
             AND Time<?
           ORDER BY Time""",
        (db.sim_id, ads_id, load_time, discharge_time),
    )

    if available.empty:
        raise RuntimeError(
            f"No `core` ExplicitInventory snapshot exists between load "
            f"time {load_time} and discharge time {discharge_time}."
        )

    snapshot_time = int(available.iloc[0]["Time"])

    snapshot = db.query(
        """SELECT NucId, SUM(Quantity) AS Quantity
           FROM ExplicitInventory
           WHERE SimId=?
             AND AgentId=?
             AND InventoryName='core'
             AND Time=?
           GROUP BY NucId""",
        (db.sim_id, ads_id, snapshot_time),
    )

    if snapshot.empty:
        raise RuntimeError(
            f"The ADS core snapshot at time {snapshot_time} is empty."
        )

    composition = (
        snapshot.groupby("NucId")["Quantity"]
        .sum()
        .astype(float)
        .sort_index()
    )
    composition = composition[composition > 0.0]

    expected_load = float(events.loc[load_time, "Load"])
    actual_load = float(composition.sum())
    tolerance = max(1.0e-6 * expected_load, 1.0e-3)

    if abs(actual_load - expected_load) > tolerance:
        raise RuntimeError(
            f"Core snapshot mass ({actual_load:.6f} kg) does not match "
            f"AdsEvents.Load ({expected_load:.6f} kg) for time {load_time}."
        )

    return composition, load_time, snapshot_time


def safe_slug(text: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9]+", "_", text.strip()).strip("_").lower()
    return slug or "ads"


def plot_before_after(
    input_groups: pd.Series,
    output_groups: pd.Series,
    case_label: str,
    load_time: int,
    discharge_time: int,
    output_path: str,
) -> None:
    """Create the two stacked bars on a linear mass scale."""

    active_groups = [
        group
        for group in GROUP_ORDER
        if input_groups[group] > 1.0e-9
        or output_groups[group] > 1.0e-9
    ]

    input_t = input_groups / 1000.0
    output_t = output_groups / 1000.0

    total_input_t = float(input_t.sum())
    total_output_t = float(output_t.sum())
    max_total_t = max(total_input_t, total_output_t)

    x = np.arange(2)
    bottoms = np.zeros(2)

    fig, ax = plt.subplots(figsize=(8.5, 6.2))

    for group in active_groups:
        values = np.array(
            [input_t[group], output_t[group]],
            dtype=float,
        )

        ax.bar(
            x,
            values,
            width=0.62,
            bottom=bottoms,
            label=group,
        )

        # Label only segments large enough to remain readable.
        for bar_index, value in enumerate(values):
            if value >= 0.035 * max_total_t:
                ax.text(
                    x[bar_index],
                    bottoms[bar_index] + value / 2.0,
                    f"{value:.2f} t",
                    ha="center",
                    va="center",
                    fontsize=8,
                )

        bottoms += values

    for bar_index, total in enumerate((total_input_t, total_output_t)):
        ax.text(
            x[bar_index],
            total + 0.025 * max_total_t,
            f"Total: {total:.3f} t",
            ha="center",
            va="bottom",
            fontsize=9,
            fontweight="bold",
        )

    ax.set_xticks(x)
    ax.set_xticklabels(["Core input", "ADS discharge"])
    ax.set_ylabel("Material mass [t]")
    ax.set_title(f"{case_label} composition before and after irradiation")
    ax.set_ylim(0.0, max_total_t * 1.14)
    ax.grid(axis="y", alpha=0.25)

    ax.legend(
        title="Material group",
        bbox_to_anchor=(1.02, 1.0),
        loc="upper left",
        fontsize=8,
    )

    fig.text(
        0.5,
        0.01,
        f"Matched cycle: load at timestep {load_time}; "
        f"discharge at timestep {discharge_time}",
        ha="center",
        fontsize=8,
    )

    fig.tight_layout(rect=(0.0, 0.04, 0.82, 1.0))
    fig.savefig(output_path, dpi=200, bbox_inches="tight")
    plt.close(fig)


def save_values_csv(
    input_groups: pd.Series,
    output_groups: pd.Series,
    csv_path: str,
) -> None:
    table = pd.DataFrame(
        {
            "group": GROUP_ORDER,
            "core_input_kg": [input_groups[group] for group in GROUP_ORDER],
            "ads_discharge_kg": [
                output_groups[group] for group in GROUP_ORDER
            ],
        }
    )

    table["change_kg"] = (
        table["ads_discharge_kg"] - table["core_input_kg"]
    )
    table.to_csv(csv_path, index=False)


def print_summary(
    input_groups: pd.Series,
    output_groups: pd.Series,
    load_time: int,
    snapshot_time: int,
    discharge_time: int,
    receiver_id: int,
    buffer_name: str,
) -> None:
    input_total = float(input_groups.sum())
    output_total = float(output_groups.sum())

    input_actinides = float(
        input_groups[
            ["Np", "Am", "Cm", "Pu", "U", "Other actinides"]
        ].sum()
    )
    output_actinides = float(
        output_groups[
            ["Np", "Am", "Cm", "Pu", "U", "Other actinides"]
        ].sum()
    )

    actinide_reduction = input_actinides - output_actinides
    reduction_percent = (
        100.0 * actinide_reduction / input_actinides
        if input_actinides > 0.0
        else float("nan")
    )

    print("\nSelected matched cycle")
    print("----------------------")
    print(f"Load event:             timestep {load_time}")
    print(f"Core snapshot:          timestep {snapshot_time}")
    print(f"Discharge event:        timestep {discharge_time}")
    print(f"Output receiver:        AgentId {receiver_id}")
    print(f"Output receiver buffer: {buffer_name}")

    print("\nMaterial-group masses [kg]")
    print("--------------------------")
    print(
        f"{'Group':<28}{'Core input':>16}{'ADS discharge':>18}"
        f"{'Change':>16}"
    )

    for group in GROUP_ORDER:
        before = float(input_groups[group])
        after = float(output_groups[group])
        print(
            f"{group:<28}{before:>16.3f}{after:>18.3f}"
            f"{after - before:>16.3f}"
        )

    print("-" * 78)
    print(
        f"{'Total tracked mass':<28}{input_total:>16.3f}"
        f"{output_total:>18.3f}{output_total - input_total:>16.3f}"
    )
    print(
        f"{'Total actinide mass':<28}{input_actinides:>16.3f}"
        f"{output_actinides:>18.3f}"
        f"{output_actinides - input_actinides:>16.3f}"
    )
    print(
        f"\nNet actinide reduction: {actinide_reduction:.3f} kg "
        f"({reduction_percent:.2f}% of input actinides)"
    )
    print(
        f"Input minus output tracked mass: "
        f"{input_total - output_total:.6f} kg"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot one matched ADS core input and ADS-only discharge as "
            "before-and-after stacked bars."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        "database",
        help="Cyclus SQLite output file.",
    )
    parser.add_argument(
        "--ads",
        default="MyADS",
        help="ADS prototype name (default: MyADS).",
    )
    parser.add_argument(
        "--case-label",
        default="ADS",
        help='Figure label, for example "ADS 1" or "ADS 2".',
    )
    parser.add_argument(
        "--output",
        default=None,
        help=(
            "Output image path. Default: "
            "<case-label>_before_after.png"
        ),
    )
    parser.add_argument(
        "--csv",
        default=None,
        help=(
            "Optional CSV path for the plotted group masses. Default: "
            "no CSV."
        ),
    )

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    output_path = args.output
    if output_path is None:
        output_path = f"{safe_slug(args.case_label)}_before_after.png"

    output_directory = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(output_directory, exist_ok=True)

    db: Optional[CyclusDatabase] = None

    try:
        db = CyclusDatabase(args.database)
        ads_id = db.find_ads(args.ads)
        events = ads_events_by_time(db, ads_id)

        (
            output_composition,
            discharge_time,
            receiver_id,
            buffer_name,
            expected_output_mass,
        ) = clean_ads_discharge(db, ads_id, events)

        (
            input_composition,
            load_time,
            snapshot_time,
        ) = matching_core_input(
            db,
            ads_id,
            events,
            discharge_time,
        )

        input_groups = aggregate_groups(input_composition)
        output_groups = aggregate_groups(output_composition)

        print_summary(
            input_groups,
            output_groups,
            load_time,
            snapshot_time,
            discharge_time,
            receiver_id,
            buffer_name,
        )

        plot_before_after(
            input_groups,
            output_groups,
            args.case_label,
            load_time,
            discharge_time,
            output_path,
        )

        print(f"\nWrote figure: {output_path}")

        if args.csv:
            csv_directory = os.path.dirname(os.path.abspath(args.csv))
            os.makedirs(csv_directory, exist_ok=True)
            save_values_csv(input_groups, output_groups, args.csv)
            print(f"Wrote values: {args.csv}")

    except (FileNotFoundError, RuntimeError, sqlite3.Error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)

    finally:
        if db is not None:
            db.close()


if __name__ == "__main__":
    main()
