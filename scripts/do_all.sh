#!/bin/bash
#
# HIPI-Board build + flash
#
# Detta är din fungerande version med EN ändring: väntetestet på BOOTSEL.
# Ingenting körs mot enheten före det testet.

set -u

PROJECT_DIR="$HOME/Projects/HIPI-Board"
DOWNLOAD_DIR="$HOME/Downloads"
PICO_MOUNT="/media/thomas/RP2350"

# Hur länge vi väntar på BOOTSEL innan vi ger upp (sekunder)
BOOTSEL_TIMEOUT=300

# Defaults
PANEL="7"
FULL_BUILD=false
ZIP_FILE=""


# ------------------------------------------------------------
# Väntetest
#
# [-d] räcker inte: udisks lämnar ofta kvar en tom katalog efter en
# urkopplad enhet, och då är loopen igenom direkt medan monteringen
# ännu inte är redo (eller är read-only). Det gav "Permission denied"
# och den tidiga kopieringen.
#
# Fem villkor, och det sista är det enda som är ett bevis:
#   1. sökvägen är en riktig monteringspunkt, inte bara en katalog
#   2. den underliggande blockenheten finns (fångar stale montering)
#   3. den är monterad read-write
#   4. INFO_UF2.TXT finns -- bootloaderns eget fingeravtryck
#   5. ett riktigt skrivtest: skapa och ta bort en probe-fil
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

pushd "$PROJECT_DIR" > /dev/null || {
    echo "FEL: Kunde inte gå till $PROJECT_DIR"
    exit 1
}


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

# Se till att datat når enheten. Efter detta startar Picon om och volymen
# försvinner -- ingenting kontrolleras efter denna rad.
sync


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
