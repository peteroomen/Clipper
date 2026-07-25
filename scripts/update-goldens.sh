#!/usr/bin/env bash
# Re-bless the M11 golden "first five minutes" rig renders (core/tests/goldens/).
#
# DELIBERATE ACT ONLY. The goldens are the project's only defence against voicing
# drift, and they are .wav files a reviewer cannot read in a diff — so re-blessing
# one is the single easiest way to turn a regression into canon. Before 2026-07-25
# this script was `cmake && ./tests --update-goldens`: no clean-tree check, no diff
# summary, no confirmation, no justification. Now it requires all four:
#
#   1. a clean working tree, so the golden diff is reviewable on its own
#   2. a printed per-golden before/after table in dB (the same third-octave band
#      deltas the gate itself uses), measured against the PREVIOUS goldens
#   3. a confirmation typed at a terminal — read from /dev/tty, so `yes | …`
#      cannot answer it and CI (no controlling terminal) cannot either
#   4. a justification, appended to core/tests/goldens/GOLDENS.md, staged with the
#      goldens so the commit carries the reason alongside the bytes
#
# Usage:
#   bash scripts/update-goldens.sh [-m "why this voicing change is correct"] [build-dir]
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_ROOT="$PWD"

GOLDEN_DIR="core/tests/goldens"
CHANGELOG="$GOLDEN_DIR/GOLDENS.md"
JUSTIFICATION=""
BUILD="build"

while [ $# -gt 0 ]; do
    case "$1" in
        -m|--message)
            JUSTIFICATION="${2:-}"
            shift 2
            ;;
        -h|--help)
            sed -n '2,26p' "$0"
            exit 0
            ;;
        *)
            BUILD="$1"
            shift
            ;;
    esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

# --- 1. Clean tree -----------------------------------------------------------
# A re-bless must be reviewable as its own diff. If the voicing change that
# justifies it is still uncommitted, the golden .wav churn and the source change
# land together and nobody can tell which byte belongs to which decision.
if ! git diff --quiet HEAD 2>/dev/null; then
    echo "Uncommitted changes to tracked files:" >&2
    git status --short >&2
    die "working tree is not clean.
       Commit the voicing change FIRST, then re-bless on top of it, so the golden
       diff is a separate reviewable commit. (Untracked files are fine.)"
fi

# --- 2. Build + measure, without writing anything ----------------------------
echo "Building the expectations suite..."
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -S core >/dev/null
cmake --build "$BUILD" -j --target clipper_player_expectations_tests >/dev/null
BIN="$BUILD/clipper_player_expectations_tests"
[ -x "$BIN" ] || die "expected test binary at $BIN"

echo "Measuring the fresh renders against the COMMITTED goldens..."
REPORT="$(mktemp)"
trap 'rm -f "$REPORT"' EXIT
"$BIN" --golden-report | tee "$REPORT" | grep -v '^GOLDEN-DELTA ' || true

# GOLDEN-DELTA <name> <status> <rmsDb> <worstBandDb> <worstHz> <bands>
DELTAS="$(grep '^GOLDEN-DELTA ' "$REPORT" || true)"
[ -n "$DELTAS" ] || die "the test binary printed no GOLDEN-DELTA lines — did --golden-report break?"

echo
printf '  %-18s %-14s %10s %14s %10s\n' 'GOLDEN' 'STATUS' 'RMS Δ' 'WORST BAND Δ' 'AT'
printf '  %-18s %-14s %10s %14s %10s\n' '------' '------' '-----' '------------' '--'
CHANGED_COUNT=0
while read -r _ name status rms worst hz _bands; do
    printf '  %-18s %-14s %9s dB %11s dB %8s Hz\n' "$name" "$status" "$rms" "$worst" "$hz"
    case "$status" in
        UNCHANGED) ;;
        *) CHANGED_COUNT=$((CHANGED_COUNT + 1)) ;;
    esac
done <<< "$DELTAS"
echo
echo "  UNCHANGED = within the 16-bit storage floor (0.15 dB); the voicing gate is 1.5 dB."

if [ "$CHANGED_COUNT" -eq 0 ]; then
    echo
    echo "Nothing to bless: every golden matches the fresh render to within the storage"
    echo "floor. The goldens are already correct — no files written."
    exit 0
fi

# --- 3. Confirmation, from the terminal and nowhere else ---------------------
# Reading /dev/tty rather than stdin is deliberate: a pipe (`yes | …`) never
# reaches it, and in CI there is no controlling terminal so the open fails.
PHRASE="bless $CHANGED_COUNT goldens"
# Probe with a simple command first: a failing redirection on `exec` can take the
# whole shell down, which would skip this message.
if ! { : < /dev/tty; } 2>/dev/null; then
    die "re-blessing goldens requires an interactive terminal (no readable /dev/tty here).
       This is intentional: it must not be possible to bless a regression from a
       script, from a pipe, or from CI."
fi
exec 3< /dev/tty

echo
echo "This will OVERWRITE $CHANGED_COUNT of the golden references above."
echo "Re-blessing is how a regression becomes canon — the diff is a .wav nobody can read."
echo
printf 'Type exactly "%s" to continue: ' "$PHRASE"
IFS= read -r REPLY_LINE <&3 || die "no answer read from the terminal — aborted, nothing written."
if [ "$REPLY_LINE" != "$PHRASE" ]; then
    die "confirmation phrase did not match — aborted, nothing written."
fi

# --- 4. Justification --------------------------------------------------------
if [ -z "$JUSTIFICATION" ]; then
    echo
    echo "Justification: what changed audibly, and why is the NEW render the correct one?"
    printf '> '
    IFS= read -r JUSTIFICATION <&3 || true
fi
if [ "${#JUSTIFICATION}" -lt 20 ]; then
    die "justification too short (${#JUSTIFICATION} chars, need >= 20) — aborted, nothing written.
       It goes into $CHANGELOG and is the only record a future session has of why
       these bytes changed. Pass it with -m \"...\" if you prefer."
fi
exec 3<&-

# --- 5. Bless ----------------------------------------------------------------
echo
echo "Rewriting goldens..."
"$BIN" --update-goldens

# --- 6. The changelog, so the reason ships with the bytes -------------------
HEAD_SHA="$(git rev-parse --short HEAD)"
HEAD_SUBJECT="$(git log -1 --format=%s)"
WHO="$(git config user.name 2>/dev/null || true)"
[ -n "$WHO" ] || WHO="${USER:-unknown}"
TODAY="$(date +%Y-%m-%d)"

ENTRY="$(mktemp)"
{
    echo "## $TODAY — $CHANGED_COUNT golden(s) re-blessed"
    echo
    echo "- **Blessed by:** $WHO"
    echo "- **On top of:** \`$HEAD_SHA\` $HEAD_SUBJECT"
    echo
    echo "| Golden | Status | Broadband RMS Δ | Worst third-octave band Δ | Bands |"
    echo "| --- | --- | --- | --- | --- |"
    while read -r _ name status rms worst hz bands; do
        echo "| \`$name\` | $status | $rms dB | $worst dB @ $hz Hz | $bands |"
    done <<< "$DELTAS"
    echo
    echo "**Justification:** $JUSTIFICATION"
    echo
} > "$ENTRY"

[ -f "$CHANGELOG" ] || die "$CHANGELOG is missing — it is a tracked file; restore it."
MARKER='<!-- New entries go directly below this line, newest first. -->'
grep -qF "$MARKER" "$CHANGELOG" || die "$CHANGELOG lost its insertion marker; restore it."

UPDATED="$(mktemp)"
while IFS= read -r line; do
    printf '%s\n' "$line"
    if [ "$line" = "$MARKER" ]; then
        echo
        cat "$ENTRY"
    fi
done < "$CHANGELOG" > "$UPDATED"
mv "$UPDATED" "$CHANGELOG"
rm -f "$ENTRY"

# Stage the bytes and the reason together. The commit must carry both — CI's
# `goldens` job fails a PR that changes a golden .wav without touching this file.
git add -- "$GOLDEN_DIR"

echo
echo "Done. Staged:"
git diff --cached --stat -- "$GOLDEN_DIR"
echo
echo "Next: listen to the renders, then commit the goldens together with the changelog"
echo "entry. Say in the PR body what changed audibly and what measurement backs it."
