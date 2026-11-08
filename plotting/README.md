# EINSTEIN thesis plotting scripts

Python scripts for analyzing **Cyclus** simulations and **Serpent** depletion results from *Simulation of Accelerator-Driven Transmutation of the U.S. Spent Nuclear Fuel Inventory* (UIUC, 2026).

## Which script do I need?

| Script | What it produces | Thesis figures |
|---|---|---|
| `plot_selection_policies.py` | Compares spent-fuel assembly selection policies and source facilities | 4.2–4.3 |
| `plot_fuel_cycle.py` | Cyclus results: inventories, ADS operating history, fuel flows, provenance, and diagnostics | 4.1, 4.8–4.9; related diagnostics |
| `plot_fission_yields_and_burnup.py` | Serpent fission-product yield curves and ADS actinide destruction per cycle | 4.4–4.7, 4.10 |
| `plot_ads_before_after.py` | Compares one ADS core loading with its discharge | 4.13–4.14 |

**Note:** Figures 4.11–4.12 are not yet confirmed to be exactly reproducible from these scripts. Figure-to-script matches are based on recovered code, not a full rerun of the original thesis data.

## Setup

```bash
python3 -m pip install numpy pandas matplotlib
```

Run the following commands from the `plotting/` folder. Replace example paths and filenames with your own. You need the matching **STANDARDS assembly files**, **Cyclus `.sqlite` output**, or **Serpent case folder**, depending on the script.

## How to run

**1. Selection policies** — uses STANDARDS files; no Cyclus run needed:

```bash
python3 plot_selection_policies.py ../data/standards --set A -o policy_figs
```

`--set A` compares highest/lowest minor-actinide content and the `first` policy. Use `--set B` for older/newer and highest/lowest burnup. Outputs: `policy_trajectory.png` and `policy_provenance.png`.

**2. General fuel-cycle plots** — uses a Cyclus SQLite database:

```bash
python3 plot_fuel_cycle.py ../runs/ma_recycle_100.sqlite \
  -o figs_ma --ads MyADS --ref 0 100 \
  --case-dir ../serpent_cases --cycle-time 120
```

For the 50/50 Pu–MA case, use `--ref 50.19 49.81 --cycle-time 24` and the corresponding SQLite file. For once-through runs, add `--sink SpentFuelStore` **only if that is the actual ADS-discharge Sink prototype**. Generates up to 10 PNG diagnostics (some are skipped if input data are missing). `--case-dir` is needed for the ADS discharge composition plot.

**3. Fission-product yields and burnup** — uses a Serpent case folder and/or Cyclus SQLite database:

```bash
python3 plot_fission_yields_and_burnup.py \
  --case-dir ../serpent_cases/CASE_NAME \
  --db ../runs/ma_recycle_100.sqlite -o figs_ma
```

Generates `11_fp_yield_curve.png` and `12_loaded_vs_transmuted.png`. You can supply **only** `--case-dir` (yield plot) or **only** `--db` (burnup plot).

**4. ADS core before/after** — uses a Cyclus SQLite database:

```bash
python3 plot_ads_before_after.py ../runs/ma_recycle_100.sqlite \
  --ads MyADS --case-label 'ADS 1' \
  --output ads1_before_after.png --csv ads1_before_after.csv
```

For ADS 2, use its SQLite file and `--case-label 'ADS 2'`. Produces a stacked-bar PNG and, optionally, a CSV of the underlying masses (omit `--csv` if unnecessary).

## Important notes

- For plots using isotopic inventories, the Cyclus input should contain `<explicit_inventory>1</explicit_inventory>` in its control block.
- `MyADS`, `SpentFuelStore`, `CASE_NAME`, and the database filenames are **examples**. Use the names and paths from your own simulation.
- The scripts read existing results; they **do not run Cyclus or Serpent**. To generate new results, the relevant archetypes, input XML, and depletion data are required.
- `plot_selection_policies.py` is an **offline policy comparison**, not an exact simulation of bids, throughput, or elapsed time. Its x-axis counts assemblies, not months.
- Check that your SQLite database contains the intended simulation: some scripts select the **newest run** if multiple runs share a file.
- The before/after script requires a clean ADS discharge snapshot in the receiving facility; it may stop with an explanatory error if one cannot be identified.

These are recovered analysis scripts. Confirm the original input datasets and plotted values before treating results as an exact reproduction of the thesis.
