#!/bin/bash

rm hipi.zip

zip -r hipi.zip . -x "build/*" ".git/*" "*.pico-sdk/*" "lif/*" "Version_1.4/*" "lib/*" "documents/*" ".vscode/*"
