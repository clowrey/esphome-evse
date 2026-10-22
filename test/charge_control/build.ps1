# Builds the host replay and scenario tests for the charge controller (needs g++ on PATH).
# Run from anywhere: .\test\charge_control\build.ps1
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$comp = Join-Path $here "..\..\components"
$core = Join-Path $comp "evse_charge_control\charge_controller.cpp"
Set-Location $here
g++ -std=c++17 -O2 -Wall -Wextra -I $comp -o replay.exe replay.cpp $core
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
g++ -std=c++17 -O2 -Wall -Wextra -I $comp -o scenarios.exe scenarios.cpp $core
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built replay.exe and scenarios.exe"
