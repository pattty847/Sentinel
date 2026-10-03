#!/bin/bash
set -euo pipefail
script=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export HOME="$scratch"
mkdir -p "$HOME/Sentinel-runtime/bin"
printf 'signed-previous\n' > "$HOME/Sentinel-runtime/bin/sentinel-server"
before=$(shasum "$HOME/Sentinel-runtime/bin/sentinel-server")

bash -n "$script"
deploy=$(/bin/bash "$script" --dry-run server)
deploy_suffix=$(/bin/bash "$script" server --dry-run)
rollback=$(/bin/bash "$script" --dry-run rollback server)
[[ $deploy == *"sentinel-server.rollback"* ]]
[[ $deploy_suffix == "$deploy" ]]
[[ $deploy == *"rollback command:"* ]]
[[ $rollback == *"verify writes within 60 s"* ]]
[[ $rollback == *"copy $HOME/Sentinel-runtime/bin/sentinel-server.rollback"* ]]
[[ $(shasum "$HOME/Sentinel-runtime/bin/sentinel-server") == "$before" ]]
[[ ! -e "$HOME/Sentinel-runtime/bin/sentinel-server.rollback" ]]
[[ ! -e "$HOME/Sentinel-runtime/bin/sentinel-server.new" ]]
