#!/bin/zsh
for m in seq par prefilter; do uptime; nice -n 10 /Volumes/T7/sentinel-worktrees/lt-claude-b2-recon-investigation/build/mac-clang/tests/servermodel/b2_recon_harness /tmp/b2-recon/raw-l2 1792278000000 $m; done
uptime; nice -n 10 /Volumes/T7/sentinel-worktrees/lt-claude-b2-recon-investigation/build/mac-clang/tests/servermodel/b2_recon_harness /tmp/b2-recon/h22only 1792278000000 par
uptime; nice -n 10 /Volumes/T7/sentinel-worktrees/lt-claude-b2-recon-investigation/build/mac-clang/tests/servermodel/b2_recon_harness /tmp/b2-recon/raw-l2 1792278000000 cmp
