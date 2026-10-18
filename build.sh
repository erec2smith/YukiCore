#!/bin/sh
cd "$(dirname "$0")"
${CC:-cc} -O2 -o tools/embed tools/embed.c || exit 1
./tools/embed src/ui.html src/ui.h || exit 1
${CC:-cc} -O2 -o YokiCore src/main.c -pthread && echo "Built: ./YokiCore"
