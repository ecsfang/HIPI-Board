#!/bin/bash

HIPI_ZIP=hipi.zip
rm hipi.zip

zip -r $HIPI_ZIP . -x@scripts/zip-exclude.txt
zip -r $HIPI_ZIP documents/*.md
zip -r $HIPI_ZIP documents/images/*.png

#zip -r hipi.zip . -x "build/*" ".git/*" "*.pico-sdk/*" "lif/*" "Version_1.4/*" "lib/*" "documents/*" ".vscode/*" "logs/*" "PicoLed/*" "./*.zip"
