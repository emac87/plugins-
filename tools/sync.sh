#!/bin/bash
# Keeps ~/Documents/EvanPlugins identical to GitHub (emac87/plugins-, branch main).
# GitHub is the one source of truth: changes are pushed there first, then this pulls them.
#   - First run: turns the folder into a git checkout of origin/main (files already here are kept;
#     tracked files are made to match GitHub).
#   - Every run: fetches, refuses if this folder has changes that aren't on GitHub, else fast-forwards.
# Exit codes: 0 synced, 2 local changes (nothing touched), 3 couldn't reach GitHub (nothing touched).
# Log: tools/sync.log. Run by tools/Sync from GitHub.command, ship_ios.sh and the Mac build scripts.
export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
cd "$(dirname "$0")/.." || exit 1
REPO=https://github.com/emac87/plugins-.git
LOG=tools/sync.log
say() { echo "$(date '+%Y-%m-%d %H:%M') $*" | tee -a "$LOG"; }

if [ ! -d .git ]; then
    git init -q -b main && git remote add origin "$REPO" || { say "FAILED: git init"; exit 1; }
    git fetch -q origin main || { say "OFFLINE: couldn't reach GitHub"; exit 3; }
    git reset -q --hard origin/main
    say "SET UP: this folder is now a git checkout of GitHub main ($(git rev-parse --short HEAD))"
    exit 0
fi

if ! git fetch -q origin main; then say "OFFLINE: couldn't reach GitHub; using the files here ($(git rev-parse --short HEAD))"; exit 3; fi

if [ -n "$(git status --porcelain)" ]; then
    say "LOCAL CHANGES: not synced. These files differ from GitHub; push them first (ask Claude):"
    git status --short | tee -a "$LOG"
    exit 2
fi

before=$(git rev-parse --short HEAD)
if git merge -q --ff-only origin/main; then
    after=$(git rev-parse --short HEAD)
    [ "$before" = "$after" ] && say "UP TO DATE ($after)" || say "SYNCED $before -> $after"
    exit 0
fi
say "DIVERGED: commits here aren't on GitHub; ask Claude to sort it out"
exit 2
