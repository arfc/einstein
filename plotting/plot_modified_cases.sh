#!/bin/bash
# Exit immediately if any command fails
set -e

echo “1. Creating the directories”
mkdir OT_eff_change
mkdir OT_serp_change
mkdir RC_eff_change
mkdir RC_serp_change


echo “2. Oncethrough efficiency change”

echo “2.1 General fuel cycle plots”
python3 plot_fuel_cycle.py ../modified_cases/OT_eff_change.sqlite -o figs_ma --ref 50.19 49.81 --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --cycle-time 24

echo “2.2 Fission product yields and burnup”
python3 plot_fission_yields_and_burnup.py --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --db ../modified_cases/OT_eff_change.sqlite -o figs_ma

echo “2.3 ADS before and after plots”
python3 plot_ads_before_after.py ../modified_cases/OT_eff_change.sqlite --case-label 'ADS 1' --output ads1_before_after.png 

mv ads1_before_after.png ./OT_eff_change
mv figs_ma ./OT_eff_change



echo “3. Recycling efficiency change”

echo “3.1 General fuel cycle plots”
python3 plot_fuel_cycle.py ../modified_cases/RC_eff_change.sqlite -o figs_ma --ref 50.19 49.81 --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --cycle-time 24

echo “3.2 Fission product yields and burnup”
python3 plot_fission_yields_and_burnup.py --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --db ../modified_cases/RC_eff_change.sqlite -o figs_ma

echo “3.3 ADS before and after plots”
python3 before_after_edit.py ../modified_cases/RC_eff_change.sqlite --case-label 'ADS 1' --output ads1_before_after.png 

mv ads1_before_after.png ./RC_eff_change
mv figs_ma ./RC_eff_change



echo “4. Oncethrough Serpent change”

echo “4.1 General fuel cycle plots”
python3 plot_fuel_cycle.py ../modified_cases/OT_serp_change.sqlite -o figs_ma  --ref 50.19 49.81 --case-dir ../ADS_serpent_data/nature_explicit_nosource_refuel --cycle-time 24

echo “4.2 Fission product yields and burnup”
python3 plot_fission_yields_and_burnup.py --case-dir ../ADS_serpent_data/nature_explicit_nosource_refuel --db ../modified_cases/OT_serp_change.sqlite -o figs_ma

echo “4.3 ADS before and after plots”
python3 plot_ads_before_after.py ../modified_cases/OT_serp_change.sqlite --case-label 'ADS 1' --output ads1_before_after.png 

mv ads1_before_after.png ./OT_serp_change
mv figs_ma ./OT_serp_change



echo “4. Recycling Serpent change”

echo “4.1 General fuel cycle plots”
python3 plot_fuel_cycle.py ../modified_cases/RC_serp_change.sqlite -o figs_ma --ref 50.19 49.81 --case-dir ../ADS_serpent_data/nature_explicit_nosource_refuel --cycle-time 24

echo “4.2 Fission product yields and burnup”
python3 plot_fission_yields_and_burnup.py --case-dir ../ADS_serpent_data/nature_explicit_nosource_refuel --db ../modified_cases/RC_serp_change.sqlite -o figs_ma

echo “4.3 ADS before and after plots”
python3 before_after_edit.py ../modified_cases/RC_serp_change.sqlite --case-label 'ADS 1' --output ads1_before_after.png 

mv ads1_before_after.png ./OT_serp_change
mv figs_ma ./OT_serp_change



echo “Consolidating…”

mkdir modified_case_graphs
mv OT_eff_change ./modified_case_graphs
mv OT_serp_change ./modified_case_graphs
mv RC_eff_change ./modified_case_graphs
mv RC_serp_change ./modified_case_graphs

echo “Finished!”
