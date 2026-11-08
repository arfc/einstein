# Serpent depletion data processing

This folder contains `extract_fission_products.py`. It converts **existing Serpent depletion results** (`*_dep.m`) into mass tables for the EINSTEIN Cyclus `ads` archetype and the fission-product plotting script. **It does not run Serpent or create plots.**

## When do I need it?

Run it **only if** a Serpent case has new or updated `*_dep.m` results and you need to create its mass tables. If the correct tables already exist, **you can skip this step**.

## How to run

Requires Python 3 only; no additional packages.

```bash
python3 extract_fission_products.py /path/to/serpent_cases/CASE_NAME
```

Run once **per case folder**, not on the parent folder containing many different cases. The script searches that case folder recursively for `*_dep.m` files and combines depletion segments in irradiation-day order.

Optional: change the atomic-number range used for the fission-product table (default: Z = 28–72):

```bash
python3 extract_fission_products.py /path/to/serpent_cases/CASE_NAME --zmin 28 --zmax 72
```

## Files produced

The script writes these files **inside the case folder**:

| File | Contents |
|---|---|
| `plot_data_14_fission_product_masses.txt` | Selected fission-product nuclide masses (default Z = 28–72) versus irradiation day |
| `plot_data_15_actinide_masses_full.txt` | Actinide nuclide masses (Z ≥ 89) versus irradiation day |

Both use kilograms and columns of the form `Step`, `Day [d]`, and `Nuclide_kg`. These are inputs to the ADS depletion-case workflow; the first file can also be used by `plot_fission_yields_and_burnup.py` in the `plotting/` folder.

## Important checks

- **Back up the two output tables first:** rerunning the script overwrites them.
- Confirm the resulting tables cover the expected irradiation days, have matching increasing time axes, and contain plausible nonzero masses.


For plotting instructions, see `../plotting/README.md`.
