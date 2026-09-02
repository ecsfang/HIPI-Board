#!/bin/bash

pushd ~/Projects/HIPI-Board

unzip -o ~/Downloads/$1 -x "scripts/*"
rm ~/Downloads/$1.zip


#rm -rf build

touch include/*
touch src/*

#cmake -DHIPI_BUILD_PANELS=5 -B build      # bara 5"
cmake -DHIPI_BUILD_PANELS=7 -B build      # bara 7"  ← det du vill just nu
#cmake -DHIPI_BUILD_PANELS=5;7 -B build    # båda (standard)
cmake --build build

cp ./build/hipi_7_pico.uf2 /media/thomas/RP2350/

popd

