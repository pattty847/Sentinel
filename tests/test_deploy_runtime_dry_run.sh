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
cat > "$scratch/mockbin/launchctl" <<'EOF'
#!/bin/bash
if [[ $MOCK_VERIFY == success ]]; then
    mkdir -p "$HOME/Library/Logs/Sentinel"
    countFile="$HOME/launch-count"
    count=0
    [[ ! -f $countFile ]] || count=$(cat "$countFile")
    count=$((count + 1))
    printf '%s\n' "$count" > "$countFile"
    log="$HOME/Library/Logs/Sentinel/sentinel-server-20990101-000000-$count.log"
    printf 'Recording v2 started\n' > "$log"
    touch -t "209901010000.0$count" "$log"
fi
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
[[ $(/bin/bash "$fixture" --dry-run rollback server) == *"verify writes within 60 s"* ]]
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
