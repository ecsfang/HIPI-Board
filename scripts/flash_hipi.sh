#!/bin/bash

PICO_MOUNT="/media/thomas/RP2350"

# Defaults
PANEL="7"

pushd ~/Projects/HIPI-Board

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

cp ./build/hipi_7_pico.uf2 /media/thomas/RP2350/

popd
