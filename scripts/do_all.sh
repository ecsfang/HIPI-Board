#!/bin/bash
#
# HIPI-Board build + flash
#
# Hanterar:
#   *.zip       -> packas upp i projektet
#   *.cpp       -> flyttas från Downloads till src/
#   *.h/*.hpp   -> flyttas från Downloads till include/
#
# Flera filer kan anges på kommandoraden, t.ex.:
#   ./do_all.sh fil1.cpp fil1.h test.zip ALL
#
# Panel:
#   5 eller 7
#
# ALL:
#   gör en komplett build
#

set -u

PROJECT_DIR="$HOME/Projects/HIPI-Board"

# Hitta användarens Downloads-katalog automatiskt.
# Kan t.ex. vara ~/Downloads eller ~/Hämtningar.
DOWNLOAD_DIR="$(xdg-user-dir DOWNLOAD)"

if [ -z "$DOWNLOAD_DIR" ] || [ ! -d "$DOWNLOAD_DIR" ]; then
    echo "FEL: Kunde inte hitta Downloads-katalogen."
    echo "Försökte använda: $DOWNLOAD_DIR"
    exit 1
fi

PICO_MOUNT="/media/thomas/RP2350"

# Hur länge vi väntar på BOOTSEL innan vi ger upp (sekunder)
BOOTSEL_TIMEOUT=300

# Defaults
PANEL="7"
FULL_BUILD=false

# Filer som skall hanteras
FILES=()

# Filer som faktiskt flyttades/kopierades in
COPIED=()


# ------------------------------------------------------------
# Väntetest
# ------------------------------------------------------------

bootsel_ready() {
    local mp="$1"
    local dev probe

    [ -d "$mp" ] || return 1

    findmnt -rno TARGET "$mp" >/dev/null 2>&1 || return 1

    dev=$(findmnt -rno SOURCE --target "$mp" 2>/dev/null)
    [ -n "$dev" ] || return 1
    [ -b "$dev" ] || return 1

    findmnt -rno OPTIONS --target "$mp" 2>/dev/null | grep -q '\brw\b' || return 1

    [ -f "$mp/INFO_UF2.TXT" ] || return 1

    probe="$mp/.probe.$$"
    ( : > "$probe" ) 2>/dev/null || return 1
    [ -e "$probe" ] || return 1
    rm -f "$probe" 2>/dev/null

    return 0
}


# ------------------------------------------------------------
# Tolka parametrar
#
# Kända parametrar:
#   5 | 7          panelval
#   ALL            komplett build
#   *.zip          projektarkiv
#   *.cpp          C++-fil
#   *.h            header
#   *.hpp          C++ header
# ------------------------------------------------------------

for ARG in "$@"; do

    case "$ARG" in

        5)
            PANEL="5"
            ;;

        7)
            PANEL="7"
            ;;

        ALL)
            FULL_BUILD=true
            ;;

        *.zip|*.ZIP)
            FILES+=("$ARG")
            ;;

        *.cpp|*.CPP)
            FILES+=("$ARG")
            ;;

        *.h|*.H)
            FILES+=("$ARG")
            ;;

        *.hpp|*.HPP)
            FILES+=("$ARG")
            ;;

        *)
            echo "FEL: Okänd parameter: $ARG"
            echo
            echo "Användning:"
            echo "  $0 [filer...] [5|7] [ALL]"
            echo
            echo "Exempel:"
            echo "  $0"
            echo "  $0 ALL"
            echo "  $0 5"
            echo "  $0 5 ALL"
            echo "  $0 test.zip"
            echo "  $0 fil1.cpp"
            echo "  $0 fil1.h"
            echo "  $0 fil1.hpp"
            echo "  $0 fil1.cpp fil1.h test.zip ALL"
            echo
            echo "Filer hämtas från:"
            echo "  $DOWNLOAD_DIR"
            echo
            echo "Hantering:"
            echo "  *.zip       -> packas upp i projektet"
            echo "  *.cpp       -> src/"
            echo "  *.h         -> include/"
            echo "  *.hpp       -> include/"
            echo
            echo "Filer från Downloads flyttas/tas bort efter lyckad hantering."
            exit 1
            ;;

    esac

done


# ------------------------------------------------------------
# Filnamn för firmware
# ------------------------------------------------------------

UF2_FILE="$PROJECT_DIR/build/hipi_${PANEL}_pico.uf2"

BUILD_LOG="/tmp/hipi_build.log"


# ------------------------------------------------------------
# Gå till projektet
# ------------------------------------------------------------

pushd "$PROJECT_DIR" > /dev/null || {
    echo "FEL: Kunde inte gå till $PROJECT_DIR"
    exit 1
}


# ------------------------------------------------------------
# Hantera alla angivna filer
#
# Varje fil behandlas individuellt:
#
#   *.zip       -> unzip i projektet
#   *.cpp       -> flyttas till ./src/
#   *.h/*.hpp   -> flyttas till ./include/
#
# Ordningen på kommandoraden spelar ingen roll.
# ------------------------------------------------------------

if [ "${#FILES[@]}" -gt 0 ]; then

    echo
    echo "=== Hanterar filer ==="

    ADDED=0

    for FILE_ARG in "${FILES[@]}"; do

        NAME=$(basename "$FILE_ARG")

        # ----------------------------------------------------
        # Leta efter filen i Downloads
        # ----------------------------------------------------

        SRC="$DOWNLOAD_DIR/$NAME"

        if [ ! -f "$SRC" ]; then

            # Tillåt även en explicit sökväg
            if [ -f "$FILE_ARG" ]; then
                SRC="$FILE_ARG"
            else
                echo
                echo "FEL: Filen finns inte:"
                echo "  $DOWNLOAD_DIR/$NAME"
                echo
                popd > /dev/null
                exit 1
            fi

        fi


        # ----------------------------------------------------
        # Bestäm filtyp och destination
        # ----------------------------------------------------

        EXT="${NAME##*.}"
        EXT="${EXT,,}"

        case "$EXT" in

            zip)
                echo
                echo "=== Packar upp $NAME ==="

                if unzip -o "$SRC" -x "scripts/*"; then

                    # Ta bort zip-filen om den låg i Downloads
                    if [ "$SRC" = "$DOWNLOAD_DIR/$NAME" ]; then
                        rm -f "$SRC"
                    fi

                    COPIED+=("$NAME -> projektet")
                    ADDED=$((ADDED + 1))

                else

                    echo
                    echo "FEL: Kunde inte packa upp $NAME"
                    echo
                    popd > /dev/null
                    exit 1

                fi
                ;;

            cpp)
                DEST="./src"
                mkdir -p "$DEST"

                echo "  $NAME -> $DEST"

                if mv -f "$SRC" "$DEST/$NAME"; then
                    COPIED+=("$NAME -> $DEST")
                    ADDED=$((ADDED + 1))
                else
                    echo
                    echo "FEL: Kunde inte flytta $NAME till $DEST"
                    echo
                    popd > /dev/null
                    exit 1
                fi
                ;;


            h|hpp)
                DEST="./include"
                mkdir -p "$DEST"

                echo "  $NAME -> $DEST"

                if mv -f "$SRC" "$DEST/$NAME"; then
                    COPIED+=("$NAME -> $DEST")
                    ADDED=$((ADDED + 1))
                else
                    echo
                    echo "FEL: Kunde inte flytta $NAME till $DEST"
                    echo
                    popd > /dev/null
                    exit 1
                fi
                ;;


            *)
                echo
                echo "FEL: Filändelsen stöds inte: $NAME"
                echo
                echo "Stödda: .zip, .cpp, .h, .hpp"
                echo
                popd > /dev/null
                exit 1
                ;;

        esac

    done

    echo
    echo "  ($ADDED fil(er) hanterade)"

else

    echo
    echo "Inga filer angivna."
    echo "Bygger befintligt projekt."

fi


# ------------------------------------------------------------
# Visa vald konfiguration
# ------------------------------------------------------------

echo
echo "============================================================"
echo " HIPI BUILD"
echo "============================================================"
echo
echo "Panel: ${PANEL}"

if [ "$FULL_BUILD" = true ]; then
    echo "Build: FULL"
else
    echo "Build: incremental"
fi

if [ "${#COPIED[@]}" -gt 0 ]; then
    echo "Filer hanterade (${#COPIED[@]}):"
    for C in "${COPIED[@]}"; do
        echo "  $C"
    done
fi

echo


# ------------------------------------------------------------
# Full build
# ------------------------------------------------------------

if [ "$FULL_BUILD" = true ]; then

    echo "=== Full build ==="

    rm -rf build

    cmake -DHIPI_BUILD_PANELS="$PANEL" -B build > "$BUILD_LOG" 2>&1

    CMAKE_RESULT=$?

    if [ "$CMAKE_RESULT" -ne 0 ]; then

        echo
        echo "============================================================"
        echo " FEL: CMAKE MISSLYCKADES"
        echo "============================================================"
        echo

        cat "$BUILD_LOG"

        echo
        popd > /dev/null
        exit 1

    fi

fi


# ------------------------------------------------------------
# Build
# ------------------------------------------------------------

echo "=== Building hipi_${PANEL}_pico ==="
echo

cmake --build build -j"$(nproc)" > "$BUILD_LOG" 2>&1 &
BUILD_PID=$!


# ------------------------------------------------------------
# Enkel spinner medan bygget pågår
# ------------------------------------------------------------

SPINNER='|/-\'
I=0

while kill -0 "$BUILD_PID" 2>/dev/null; do

    printf "\rBuilding... %c" "${SPINNER:I%4:1}"

    I=$((I + 1))

    sleep 0.15

done


# ------------------------------------------------------------
# Hämta resultat från build-processen
# ------------------------------------------------------------

wait "$BUILD_PID"
BUILD_RESULT=$?


# ------------------------------------------------------------
# Kontrollera build-resultat
# ------------------------------------------------------------

if [ "$BUILD_RESULT" -ne 0 ]; then

    echo
    echo
    echo "============================================================"
    echo " FEL: BYGGET MISSLYCKADES"
    echo "============================================================"
    echo
    echo "Build-logg:"
    echo

    cat "$BUILD_LOG"

    echo
    echo "============================================================"

    popd > /dev/null
    exit 1

fi

printf "\rBuilding... KLART!\n"


# ------------------------------------------------------------
# Kontrollera UF2-filen
# ------------------------------------------------------------

if [ ! -f "$UF2_FILE" ]; then

    echo
    echo "============================================================"
    echo " FEL: UF2-FILEN SKAPADES INTE"
    echo "============================================================"
    echo
    echo "Förväntad fil:"
    echo "  $UF2_FILE"
    echo

    popd > /dev/null
    exit 1

fi


# ------------------------------------------------------------
# Vänta på Pico BOOTSEL
# ------------------------------------------------------------

echo
echo "=== Väntar på Pico 2 i BOOTSEL-läge ==="
echo
echo "Panel:    ${PANEL}"
echo "Firmware: hipi_${PANEL}_pico.uf2"
echo
echo "Sätt Pico 2 i BOOTSEL om den inte redan är det."


ELAPSED=0

while ! bootsel_ready "$PICO_MOUNT"; do

    sleep 1
    ELAPSED=$((ELAPSED + 1))

    if [ "$BOOTSEL_TIMEOUT" -gt 0 ] && [ "$ELAPSED" -ge "$BOOTSEL_TIMEOUT" ]; then

        echo
        echo "Timeout efter ${BOOTSEL_TIMEOUT}s: ingen Pico i BOOTSEL på"
        echo "  $PICO_MOUNT"
        echo

        if [ -d "$PICO_MOUNT" ] \
           && ! findmnt -rno TARGET "$PICO_MOUNT" >/dev/null 2>&1; then
            echo "Katalogen finns men är inte monterad (kvarlevande katalog)."
            echo "Testa:  udisksctl mount -b /dev/disk/by-label/RP2350"
        else
            echo "Kontrollera att Picon är i BOOTSEL och att kabeln klarar data."
        fi

        echo
        popd > /dev/null
        exit 1

    fi

done


echo
echo "Pico hittad och skrivbar: $PICO_MOUNT"


# ------------------------------------------------------------
# Flash
# ------------------------------------------------------------

echo
echo "=== Flashar firmware ==="

if ! cp "$UF2_FILE" "$PICO_MOUNT/"; then
    echo
    echo "FEL: kopieringen misslyckades. Är enheten kvar i BOOTSEL?"
    popd > /dev/null
    exit 1
fi

# Se till att datat når enheten.
# Efter detta startar Picon om och volymen försvinner.
sync


# ------------------------------------------------------------
# Klart
# ------------------------------------------------------------

echo
echo "============================================================"
echo " KLART!"
echo "============================================================"
echo
echo "Panel:    ${PANEL}"
echo "Firmware: hipi_${PANEL}_pico.uf2"
echo

if [ "${#COPIED[@]}" -gt 0 ]; then
    echo "Filer hanterade (${#COPIED[@]}):"
    for C in "${COPIED[@]}"; do
        echo "  $C"
    done
    echo
fi

popd > /dev/null
