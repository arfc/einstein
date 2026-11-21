#!/bin/bash

SECONDS = 0

echo "Started at: $(date +%T)"

echo "Removing old runs"
rm case3_OT.sqlite
rm case3_RC.sqlite
rm case4_OT.sqlite
rm case4_RC.sqlite

echo "3.O: 100% Actinides, 100 years, once-through"
cyclus -i case3_oncethrough.xml -o case3_OT.sqlite

duration=$SECONDS
echo "Case 3 once-through finished at $((duration / 60))m $((duration % 60))s."


echo "3.R: 100% Actinides, 100 years, recycling loop-back"
cyclus -i case3_recycle.xml -o case3_RC.sqlite

duration=$SECONDS
echo "Case 3 recycling finished in $((duration / 60))m $((duration % 60))s."


echo "4.O: 50% Actinides and 50% Plutonium, 30 years, once-through"
cyclus -i case4_oncethrough.xml -o case4_OT.sqlite

duration=$SECONDS
echo "Case 4 once-through finished in $((duration / 60))m $((duration % 60))s."


echo "4.R: 50% Actinides and 50% Plutonium, 30 years, recycling loop-back"
cyclus -i case4_recycle.xml -o case4_RC.sqlite

duration=$SECONDS
echo "Case 4 recycling finished in $((duration / 60))m $((duration % 60))s."


echo "All four done"

duration=$SECONDS
echo "All simulations finished in $((duration / 60))m $((duration % 60))s."
