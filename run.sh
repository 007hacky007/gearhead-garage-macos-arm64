#!/bin/sh
# Development run: ./run.sh [seconds]
cd "$(dirname "$0")"
mkdir -p out/saves
GEARHEAD_TIMEOUT=${1:-20} GEARHEAD_FULLSCREEN=0 GEARHEAD_SAVE_DIR=out/saves ./out/gearhead out/linked.exe local/game > out/run.log 2>&1
echo "exit $?"
