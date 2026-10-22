#!/bin/sh
# Builds the host replay and scenario tests (Linux / WSL; on Windows use build.ps1 with MinGW).
#   wsl -e sh -c "cd /mnt/c/Users/<you>/Documents/GitHub/esphome-evse/test/charge_control && ./build.sh"
set -e
cd "$(dirname "$0")"
COMP=../../components
CORE=$COMP/evse_charge_control/charge_controller.cpp
g++ -std=c++17 -O2 -Wall -Wextra -I "$COMP" -o replay replay.cpp "$CORE"
g++ -std=c++17 -O2 -Wall -Wextra -I "$COMP" -o scenarios scenarios.cpp "$CORE"
echo "built replay and scenarios"
