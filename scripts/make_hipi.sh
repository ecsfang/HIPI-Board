#!/bin/bash

pushd ~/Projects/HIPI-Board

rm -rf build

cmake -B build                           # hipi_7_pico
cmake --build build

popd
