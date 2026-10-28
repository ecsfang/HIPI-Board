#!/bin/bash
#
# pack_hipi.sh -- packs the HIPI project into a zip with only what's needed
# to build the firmware, run the PC tests (tools/hipiview) and update the
# documents. Replaces bkup.sh.
#
#   scripts/pack_hipi.sh                 -> hipi.zip in the pack folder (below)
#   scripts/pack_hipi.sh -o x.zip        -> another name (a relative one in
#                                           the pack folder)
#   scripts/pack_hipi.sh -d DIR          -> another pack folder
#   scripts/pack_hipi.sh -c              -> only files changed since the last
#                                           full pack (hipi-changes.zip)
#   scripts/pack_hipi.sh --pcb           -> pcb/ too (schematics, BOM, photos)
#   scripts/pack_hipi.sh -n              -> list what would be packed, and the
#                                           largest files, without packing
#
# Left out:
#   - build output, IDE/tool folders, logs, zips, firmware images
#   - programs built on the PC (any ELF file: hipiview, ilctrl, *_test ...)
#     and the PC tests' output (tools/*/*.png, *.hvlog, *.mp4, *_sd/)
#   - local copies: HIPI-SD-card/ (made from sdcard/ + resources/), lif/,
#     PicoLed/, pyilper2/
#   - pcb/ (unless --pcb), documents/*.pdf
#
# The pack folder is OUTSIDE the project -- $HIPI_PACK_DIR, else
# ~/hipi-packs -- so cleaning the project folder (git clean, a new clone)
# loses neither the zips nor the time of the last full pack, which -c needs.
# (An older .last_pack in the project folder is moved there.)
#
# Works from anywhere: the project folder is the one above scripts/.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

OUT=""
PACK_DIR="${HIPI_PACK_DIR:-$HOME/hipi-packs}"
CHANGES=false
WITH_PCB=false
DRY=false

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT="$2"; shift 2 ;;
        -d) PACK_DIR="$2"; shift 2 ;;
        -c|--changes) CHANGES=true; shift ;;
        --pcb) WITH_PCB=true; shift ;;
        -n|--dry-run) DRY=true; shift ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (try -h)"; exit 1 ;;
    esac
done

if [ -z "$OUT" ]; then
    if $CHANGES; then OUT="hipi-changes.zip"; else OUT="hipi.zip"; fi
fi
mkdir -p "$PACK_DIR" || { echo "Can't make $PACK_DIR"; exit 1; }
PACK_DIR="$(cd "$PACK_DIR" && pwd)"
case "$OUT" in /*) ;; *) OUT="$PACK_DIR/$OUT" ;; esac

# When the last full pack was made: its file's time (in the pack folder)
STAMP="$PACK_DIR/.last_pack"
if [ ! -f "$STAMP" ] && [ -f "$ROOT/.last_pack" ]; then
    mv "$ROOT/.last_pack" "$STAMP" && echo "Moved .last_pack to $PACK_DIR"
fi

# Folders never packed (anywhere in the tree unless they start with ./)
PRUNE=(
    ./.git ./.vscode ./.cache ./.pico-sdk ./build ./Build ./logs ./lif
    ./HIPI-SD-card ./PicoLed ./PicoLED ./pyilper2 ./Testing
    ./tools/hipiview/build-win ./tools/hipiview/gen
    CMakeFiles 'build-*' '*_sd'
)
$WITH_PCB || PRUNE+=(./pcb)

# Files never packed
SKIP=(
    '*.o' '*.a' '*.d' '*.obj' '*.elf' '*.uf2' '*.bin' '*.hex' '*.map' '*.dis'
    '*.zip' '*.tar.gz' '*.log' '*.tmp' '*.bak' '*.swp' '*.hvlog' '*.mp4'
    CMakeCache.txt compile_commands.json .last_pack
)

prune_expr=()
for p in "${PRUNE[@]}"; do
    case "$p" in
        ./*) prune_expr+=(-path "$p" -o) ;;
        *)   prune_expr+=(-name "$p" -o) ;;
    esac
done
unset 'prune_expr[${#prune_expr[@]}-1]'

skip_expr=()
for s in "${SKIP[@]}"; do skip_expr+=(-name "$s" -o); done
skip_expr+=(-path './documents/*.pdf' -o -path './tools/*/*.png')
# (tools/images/*.png is a source picture: kept below)

newer=()
if $CHANGES; then
    if [ ! -f "$STAMP" ]; then
        echo "No full pack made yet ($STAMP missing): run without -c first."
        exit 1
    fi
    newer=(-newer "$STAMP")
fi

LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT

find . \( "${prune_expr[@]}" \) -prune -o -type f \
     ! \( "${skip_expr[@]}" \) "${newer[@]}" -print |
while IFS= read -r f; do
    # Programs built here (ELF) are never needed
    [ "$(head -c 4 "$f" 2>/dev/null | od -An -c | tr -d ' ')" = '177ELF' ] && continue
    printf '%s\n' "${f#./}"
done > "$LIST"

# Source pictures under tools/images are kept (only the tests' output is skipped)
if [ -d tools/images ]; then
    find tools/images -type f -name '*.png' ${newer[@]+"${newer[@]}"} >> "$LIST"
fi
sort -u -o "$LIST" "$LIST"

COUNT=$(wc -l < "$LIST")
BYTES=$(tr '\n' '\0' < "$LIST" | xargs -0 -r du -cb 2>/dev/null | tail -1 | cut -f1)
BYTES=${BYTES:-0}

if $DRY; then
    cat "$LIST"
    echo
    echo "Largest:"
    tr '\n' '\0' < "$LIST" | xargs -0 -r du -b | sort -n | tail -10 |
        awk '{ printf "  %8.1f kB  %s\n", $1 / 1024, $2 }'
    awk -v n="$COUNT" -v b="$BYTES" 'BEGIN { printf "%d files, %.1f MB (before zip)\n", n, b / 1048576 }'
    exit 0
fi

if [ "$COUNT" -eq 0 ]; then
    echo "Nothing to pack."
    exit 0
fi

rm -f "$OUT"
zip -q -X "$OUT" -@ < "$LIST" || { echo "zip failed"; exit 1; }
$CHANGES || touch "$STAMP"

awk -v f="$OUT" -v n="$COUNT" -v b="$BYTES" -v z="$(stat -c %s "$OUT")" \
    'BEGIN { printf "%s: %d files, %.1f MB -> %.1f MB\n", f, n, b / 1048576, z / 1048576 }'
