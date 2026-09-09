#!/bin/bash

set -u

PROJECT_DIR="$HOME/Projects/HIPI-Board"
DOWNLOAD_DIR="$HOME/Downloads"
PICO_MOUNT="/media/thomas/RP2350"

# Defaults
PANEL="7"
FULL_BUILD=false
ZIP_FILE=""


# ------------------------------------------------------------
# Tolka parametrar
#
# Tillåt:
#   zip-fil
#   5 eller 7
#   ALL
#
# Argumenten kan anges i valfri ordning.
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

        *.zip)
            ZIP_FILE="$ARG"
            ;;

        *)
            echo "FEL: Okänd parameter: $ARG"
            echo
            echo "Användning:"
            echo "  $0 [zip-fil] [5|7] [ALL]"
            echo
            echo "Exempel:"
            echo "  $0"
            echo "  $0 ALL"
            echo "  $0 5"
            echo "  $0 5 ALL"
            echo "  $0 projekt.zip"
            echo "  $0 projekt.zip ALL"
            echo "  $0 projekt.zip 5"
            echo "  $0 projekt.zip 5 ALL"
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

pushd "$PROJECT_DIR" > /dev/null


# ------------------------------------------------------------
# Packa upp projekt om en zip-fil angivits
# ------------------------------------------------------------

if [ -n "$ZIP_FILE" ]; then

    ZIP_PATH="$DOWNLOAD_DIR/$ZIP_FILE"

    if [ -f "$ZIP_PATH" ]; then

        echo
        echo "=== Packar upp $ZIP_FILE ==="

        unzip -o "$ZIP_PATH" -x "scripts/*"

        rm "$ZIP_PATH"

    else

        echo
        echo "FEL: Filen finns inte:"
        echo "  $ZIP_PATH"
        echo

        read -n 1 -s -r -p "Tryck på en tangent för att fortsätta..."
        echo

    fi

else

    echo
    echo "Ingen zip-fil angiven."
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
echo "Panel: ${PANEL}\""

if [ "$FULL_BUILD" = true ]; then
    echo "Build: FULL"
else
    echo "Build: incremental"
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
echo "Panel:    ${PANEL}\""
echo "Firmware: hipi_${PANEL}_pico.uf2"
echo
echo "Sätt Pico 2 i BOOTSEL om den inte redan är det."


while [ ! -d "$PICO_MOUNT" ]; do
    sleep 1
done


echo
echo "Pico hittad: $PICO_MOUNT"


# ------------------------------------------------------------
# Flash
# ------------------------------------------------------------

echo
echo "=== Flashar firmware ==="

cp "$UF2_FILE" "$PICO_MOUNT/"


# ------------------------------------------------------------
# Klart
# ------------------------------------------------------------

echo
echo "============================================================"
echo " KLART!"
echo "============================================================"
echo
echo "Panel:    ${PANEL}\""
echo "Firmware: hipi_${PANEL}_pico.uf2"
echo


popd > /dev/null