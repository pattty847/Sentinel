#!/bin/bash
set -euo pipefail
script=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export HOME="$scratch/home"
fixture="$scratch/repo/scripts/dev/deploy-runtime.sh"
mkdir -p "$(dirname "$fixture")" "$scratch/mockbin" "$HOME/Sentinel-runtime/bin"
cp "$script" "$fixture"
dst="$HOME/Sentinel-runtime/bin/sentinel-server"
rollback="$dst.rollback"
src="$scratch/repo/build/mac-clang/apps/sentinel-server/sentinel-server"
mkdir -p "$(dirname "$src")"
printf 'current signed binary\n' > "$dst"
printf 'older signed binary\n' > "$rollback"
printf 'candidate binary\n' > "$src"
chmod +x "$dst" "$rollback" "$src"

# This copied script points only at the fixture. The service/signature commands
# are mocks, so neither launchd nor the owner's runtime can be touched.
# launchd mock: every kickstart starts a new PID whose run log carries the
# PID/exe header (deploy-runtime.sh verifies that process only). With
# MOCK_VERIFY=success it writes the write marker; otherwise it never does.
cat > "$scratch/mockbin/launchctl" <<'EOF'
#!/bin/bash
pidFile="$HOME/launch-pid"
if [[ $1 == print ]]; then
    [[ ! -f $pidFile ]] || printf '\tpid = %s\n' "$(cat "$pidFile")"
    exit 0
fi
[[ $1 == kickstart ]] || exit 0
mkdir -p "$HOME/Library/Logs/Sentinel"
pid=1000
[[ ! -f $pidFile ]] || pid=$(cat "$pidFile")
pid=$((pid + 1))
printf '%s\n' "$pid" > "$pidFile"
log="$HOME/Library/Logs/Sentinel/sentinel-server-20990101-000000-$pid.log"
{
    printf '# Sentinel log: app=sentinel-server version=test pid=%s\n' "$pid"
    printf '# started=2099-01-01T00:00:00.000\n'
    printf '# exe=%s built=2099-01-01T00:00:00\n' "$HOME/Sentinel-runtime/bin/sentinel-server"
    [[ $MOCK_VERIFY != success ]] || printf 'Recording v2 started\n'
} > "$log"
EOF
cat > "$scratch/mockbin/codesign" <<'EOF'
#!/bin/bash
exit 0
EOF
cat > "$scratch/mockbin/sleep" <<'EOF'
#!/bin/bash
exit 0
EOF
cat > "$scratch/mockbin/git" <<'EOF'
#!/bin/bash
printf 'fixture\n'
EOF
chmod +x "$scratch/mockbin"/*
export PATH="$scratch/mockbin:/usr/bin:/bin"
export MOCK_VERIFY=fail
if /bin/bash "$fixture" server > "$scratch/first-failure.out" 2>&1; then exit 1; fi
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'older signed binary' ]]
export MOCK_VERIFY=success

bash -n "$script"
deploy=$(/bin/bash "$fixture" --dry-run server)
[[ $deploy == *"on success move $dst.prev to $rollback"* ]]
[[ $deploy == *"rollback command:"* ]]
[[ $(/bin/bash "$fixture" server --dry-run) == "$deploy" ]]
[[ $(/bin/bash "$fixture" --dry-run rollback server) == *"verify writes within the window (server 150 s, capture 60 s)"* ]]
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'older signed binary' ]]

if /bin/bash "$fixture" --dry-run > "$scratch/noargs.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/noargs.out") == *"usage:"* ]]
[[ $(cat "$scratch/noargs.out") != *"unbound variable"* ]]
mv "$src" "$src.hidden"
if /bin/bash "$fixture" --dry-run server > "$scratch/missing.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/missing.out") == *"missing $src"* ]]
mv "$src.hidden" "$src"
mv "$rollback" "$rollback.hidden"
if /bin/bash "$fixture" --dry-run rollback server > "$scratch/missing.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/missing.out") == *"missing rollback $rollback"* ]]
mv "$rollback.hidden" "$rollback"
mv "$dst" "$dst.hidden"
if /bin/bash "$fixture" --dry-run server > "$scratch/missing.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/missing.out") == *"missing deployed $dst"* ]]
mv "$dst.hidden" "$dst"

/bin/bash "$fixture" server > "$scratch/success.out"
[[ $(cat "$dst") == 'candidate binary' ]]
[[ $(cat "$rollback") == 'current signed binary' ]]
[[ $(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED") -eq 1 ]]
[[ $(cat "$HOME/Sentinel-runtime/bin/DEPLOYED") == *' deploy sentinel-server' ]]

/bin/bash "$fixture" rollback server > "$scratch/rollback.out"
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'current signed binary' ]]
[[ $(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED") -eq 2 ]]
[[ $(tail -1 "$HOME/Sentinel-runtime/bin/DEPLOYED") == *' rollback sentinel-server' ]]

printf 'older signed binary\n' > "$rollback"
printf 'new candidate binary\n' > "$src"
export MOCK_VERIFY=fail
if /bin/bash "$fixture" server > "$scratch/failure.out" 2>&1; then exit 1; fi
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'older signed binary' ]]
[[ $(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED") -eq 2 ]]

# restart: config-only restart of the deployed binary (R2 runbook, owner decision B 2026-10-07).
[[ $(/bin/bash "$fixture" --dry-run restart server) == *"restart com.sentinel.recorder with the deployed"*"no binary change"* ]]
if /bin/bash "$fixture" restart both > "$scratch/restart-both.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/restart-both.out") == *"usage:"* ]]
deployed_before=$(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED")
export MOCK_VERIFY=fail
if /bin/bash "$fixture" restart server > "$scratch/restart-fail.out" 2>&1; then exit 1; fi
[[ $(cat "$scratch/restart-fail.out") == *"restart FAILED"*"binary is unchanged"* ]]
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'older signed binary' ]]
[[ $(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED") -eq $deployed_before ]]
export MOCK_VERIFY=success
/bin/bash "$fixture" restart server > "$scratch/restart-ok.out" 2>&1
[[ $(cat "$scratch/restart-ok.out") == *"restart complete for sentinel-server (binary unchanged)"* ]]
[[ $(cat "$dst") == 'current signed binary' ]]
[[ $(cat "$rollback") == 'older signed binary' ]]
[[ $(wc -l < "$HOME/Sentinel-runtime/bin/DEPLOYED") -eq $((deployed_before + 1)) ]]
tail -1 "$HOME/Sentinel-runtime/bin/DEPLOYED" | grep -q ' restart sentinel-server$'
