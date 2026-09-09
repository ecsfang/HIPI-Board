#!/bin/bash

set -e

PROJECT_DIR="$HOME/Projects/HIPI-Board"
DOWNLOAD_DIR="$HOME/Downloads"
PICO_MOUNT="/media/thomas/RP2350"
UF2_FILE="$PROJECT_DIR/build/hipi_7_pico.uf2"
BUILD_LOG="/tmp/hipi_build.log"


# ------------------------------------------------------------
# Gå till projektet
# ------------------------------------------------------------

pushd "$PROJECT_DIR" > /dev/null


# ------------------------------------------------------------
# Packa upp projekt om en zip-fil angivits
# ------------------------------------------------------------

if [ -n "$1" ]; then

    ZIP_FILE="$DOWNLOAD_DIR/$1"

    if [ -f "$ZIP_FILE" ]; then

        echo
        echo "=== Packar upp $1 ==="

        unzip -o "$ZIP_FILE" -x "scripts/*"

        rm "$ZIP_FILE"

    else

        echo
        echo "FEL: Filen finns inte:"
        echo "  $ZIP_FILE"
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
# Full eller incremental build
# ------------------------------------------------------------

if [ "$2" = "ALL" ]; then

    echo
    echo "=== Full build ==="

    rm -rf build

    cmake -DHIPI_BUILD_PANELS=7 -B build > "$BUILD_LOG" 2>&1

else

    echo
    echo "=== Incremental build ==="

    # Tvinga ombyggnad av projektets källkod
    touch include/*
    touch src/*

fi


# ------------------------------------------------------------
# Build med progress-indikator
# ------------------------------------------------------------

echo
echo "=== Building ==="

# Kör bygget i bakgrunden och spara all output
cmake --build build -j"$(nproc)" > "$BUILD_LOG" 2>&1 &
BUILD_PID=$!


# Enkel progress-indikator
SPINNER='|/-\'
I=0

while kill -0 "$BUILD_PID" 2>/dev/null; do

    printf "\rBuilding... %c" "${SPINNER:I%4:1}"

    I=$((I + 1))

    sleep 0.15

done


# Hämta resultatet från build-processen
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
# Kontrollera att UF2-filen skapades
# ------------------------------------------------------------

if [ ! -f "$UF2_FILE" ]; then

    echo
    echo "FEL: UF2-filen skapades inte:"
    echo "  $UF2_FILE"

    popd > /dev/null
    exit 1

fi


# ------------------------------------------------------------
# Vänta på Pico BOOTSEL
# ------------------------------------------------------------

echo
echo "=== Väntar på Pico 2 i BOOTSEL-läge ==="
echo "Sätt Pico 2 i BOOTSEL om den inte redan är det."

while [ ! -d "$PICO_MOUNT" ]; do
    sleep 1
done

echo "Pico hittad: $PICO_MOUNT"


# ------------------------------------------------------------
# Kopiera firmware
# ------------------------------------------------------------

echo
echo "=== Flashar firmware ==="

cp "$UF2_FILE" "$PICO_MOUNT/"


# ------------------------------------------------------------
# Klart
# ------------------------------------------------------------

echo
echo "=== KLART ==="
echo "Firmware flashad."

popd > /dev/null