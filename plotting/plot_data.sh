#!/bin/bash
# Exit immediately if any command fails
set -e

echo "3.0. Creating the directories..."
mkdir case3_oncethrough
mkdir case3_recycle

echo "3.A. Once-through case"

echo "3.1. Running selection policies plots..."
python3 plot_selection_policies.py ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P --set A -o policy_figs

echo "3.2. Running general fuel-cycle plots..."
python3 plot_fuel_cycle.py ../examples/case3_OT.sqlite \
  -o figs_ma --ads MyADS --ref 0 100 \
  --case-dir ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P --cycle-time 120

echo "3.3. Running fission-product yields and burnup plots..."
python3 plot_fission_yields_and_burnup.py \
  --case-dir ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P \
  --db ../examples/case3_OT.sqlite -o figs_ma

echo "3.4. Running ADS core before/after plots..."
python3 plot_ads_before_after.py ../examples/case3_OT.sqlite \
  --ads MyADS --case-label 'ADS 1' \
  --output ads1_before_after.png 

mv ads1_before_after.png ./case3_oncethrough/
mv figs_ma ./case3_oncethrough/
mv policy_figs ./case3_oncethrough/


echo "3.B. Recycle case"

echo "3.1. Running selection policies plots..."
python3 plot_selection_policies.py ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P --set A -o policy_figs

echo "3.2. Running general fuel-cycle plots..."
python3 plot_fuel_cycle.py ../examples/case3_RC.sqlite \
  -o figs_ma --ads MyADS --ref 0 100 \
  --case-dir ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P --cycle-time 120

echo "3.3. Running fission-product yields and burnup plots..."
python3 plot_fission_yields_and_burnup.py \
  --case-dir ../examples/serpent_cases/no_source_892_vt_ma_Pb_3GW_fix_P \
  --db ../examples/case3_RC.sqlite -o figs_ma

echo "3.4. Running ADS core before/after plots..."
python3 plot_ads_before_after.py ../examples/case3_RC.sqlite \
  --ads MyADS --case-label 'ADS 1' \
  --output ads1_before_after.png 

mv ads1_before_after.png ./case3_recycle/
mv figs_ma ./case3_recycle/
mv policy_figs ./case3_recycle/

echo "Case 3 scripts finished successfully"



echo "4.0. Creating the directories..."
mkdir case4_oncethrough
mkdir case4_recycle

echo "4.A. Once-through case"

echo "4.1. Running selection policies plots..."
python3 plot_selection_policies.py ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --set A -o policy_figs

echo "4.2. Running general fuel-cycle plots..."
python3 plot_fuel_cycle.py ../examples/case4_OT.sqlite \
  -o figs_ma --ads MyADS --ref 50.19 49.81 \
  --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --cycle-time 24

echo "4.3. Running fission-product yields and burnup plots..."
python3 plot_fission_yields_and_burnup.py \
  --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix \
  --db ../examples/case4_OT.sqlite -o figs_ma

echo "4.4. Running ADS core before/after plots..."
python3 plot_ads_before_after.py ../examples/case4_OT.sqlite \
  --ads MyADS --case-label 'ADS 1' \
  --output ads1_before_after.png 

mv ads1_before_after.png ./case4_oncethrough/
mv figs_ma ./case4_oncethrough/
mv policy_figs ./case4_oncethrough/


echo "4.B. Recycle case"

echo "4.1. Running selection policies plots..."
python3 plot_selection_policies.py ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --set A -o policy_figs

echo "4.2. Running general fuel-cycle plots..."
python3 plot_fuel_cycle.py ../examples/case4_RC.sqlite \
  -o figs_ma --ads MyADS --ref 50.19 49.81 \
  --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix --cycle-time 24

echo "4.3. Running fission-product yields and burnup plots..."
python3 plot_fission_yields_and_burnup.py \
  --case-dir ../examples/serpent_cases/no_source_3GW_1.3_vt_5050_puma_Pb_mix \
  --db ../examples/case4_RC.sqlite -o figs_ma

echo "4.4. Running ADS core before/after plots..."
python3 before_after_edit.py ../examples/case4_RC.sqlite --ads MyADS --case-label 'ADS 1' --output ads1_before_after.png

mv ads1_before_after.png ./case4_recycle/
mv figs_ma ./case4_recycle/
mv policy_figs ./case4_recycle/

echo "Case 4 scripts finished successfully"

echo "Consolidating..."

mkdir all_cases_34
mv case3_oncethrough ./all_cases_34/
mv case3_recycle ./all_cases_34/
mv case4_oncethrough ./all_cases_34/
mv case4_recycle ./all_cases_34/

echo "All plotting scripts finished successfully!"
