# Contributing

## Layout

- `src/ui.html` is the whole editor UI. Edit this file.
- `src/ui.h` is generated from it. Commit it, the installer downloads it.
- `src/main.c` is the local server and the HTML/CSS/JS exporter.
- `tools/embed.c` turns `ui.html` into `ui.h`.
- `tests/run.sh` starts the server and checks every route.

## Build and test

    make
    make test

`make` regenerates `src/ui.h` whenever `src/ui.html` changes. CI fails if the committed header is out of date.

On Windows without make, run `build.bat`.

## Rules

- The editor language stays English.
- Keep the orange palette (`--o`, `--o2`, `--ob`, `--ol` in the CSS).
- No dependencies. The server is plain C and the UI is one HTML file.
- Server routes that change anything must check the token.
- New server behaviour needs a check in `tests/run.sh`.

## The installer

`setup/YokiCore-Setup.sh`, `.command` and `.bat` must stay byte for byte identical. After editing one, copy it over the other two. CI compares them. Change `REPO` at the top (and in the PowerShell block) when the repository moves.
