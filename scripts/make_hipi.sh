#!/bin/bash

pushd ~/Projects/HIPI-Board

rm -rf build

#cmake -DHIPI_BUILD_PANELS=5 -B build      # bara 5"
cmake -DHIPI_BUILD_PANELS=7 -B build      # bara 7"  ← det du vill just nu
#cmake -DHIPI_BUILD_PANELS=5;7 -B build    # båda (standard)
cmake --build build

popd
