:<<'YOKI_BAT'
@echo off
title YokiCore Setup
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p='%~f0'; $t=[IO.File]::ReadAllText($p); $i=$t.LastIndexOf('#'+'PS-BEGIN'); Invoke-Expression $t.Substring($i)"
echo.
pause
exit /b
YOKI_BAT
REPO="OWNER/YokiCore"
BRANCH="main"
RAW="https://raw.githubusercontent.com/$REPO/$BRANCH"
APP="${YOKICORE_APP:-$HOME/YokiCore/app}"
OS=$(uname -s)

say() { printf '%s\n' "$*"; }
die() { printf 'Setup failed: %s\n' "$*" >&2; printf 'Press Enter to close.\n'; read _ </dev/tty 2>/dev/null; exit 1; }

find_cc() {
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC=$c; return 0; fi
    done
    return 1
}

install_cc() {
    say "No C compiler found. Trying to install one."
    if [ "$OS" = "Darwin" ]; then
        xcode-select --install 2>/dev/null
        die "Finish the Command Line Tools installation that just opened, then run this file again."
    fi
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    if command -v apt-get >/dev/null 2>&1; then $SUDO apt-get update && $SUDO apt-get install -y gcc libc6-dev
    elif command -v dnf >/dev/null 2>&1; then $SUDO dnf install -y gcc glibc-devel
    elif command -v pacman >/dev/null 2>&1; then $SUDO pacman -S --noconfirm gcc
    elif command -v zypper >/dev/null 2>&1; then $SUDO zypper install -y gcc glibc-devel
    elif command -v apk >/dev/null 2>&1; then $SUDO apk add gcc musl-dev
    else die "Install gcc or clang with your package manager and run this file again."
    fi
    find_cc || die "The compiler install did not work."
}

fetch() {
    if [ -n "$YOKICORE_SRC" ]; then cp "$YOKICORE_SRC/$1" "$2"
    elif command -v curl >/dev/null 2>&1; then curl -fsSL "$RAW/$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -q "$RAW/$1" -O "$2"
    else die "Neither curl nor wget is installed."
    fi
}

find_cc || install_cc
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t yokicore)
trap 'rm -rf "$WORK"' EXIT INT TERM

say "Downloading source..."
fetch src/main.c "$WORK/main.c" || die "Could not download src/main.c from $RAW"
fetch src/ui.h "$WORK/ui.h" || die "Could not download src/ui.h from $RAW"

say "Building..."
"$CC" -O2 -o "$WORK/YokiCore" "$WORK/main.c" -pthread || die "The build failed."

mkdir -p "$APP" || die "Cannot create $APP"
cp "$WORK/YokiCore" "$APP/YokiCore" && chmod +x "$APP/YokiCore" || die "Cannot write to $APP"

if [ "$OS" = "Darwin" ]; then
    BUNDLE="$HOME/Applications/YokiCore.app"
    rm -rf "$BUNDLE"
    mkdir -p "$BUNDLE/Contents/MacOS"
    cp "$APP/YokiCore" "$BUNDLE/Contents/MacOS/YokiCore"
    cat > "$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
<key>CFBundleName</key><string>YokiCore</string>
<key>CFBundleExecutable</key><string>YokiCore</string>
<key>CFBundleIdentifier</key><string>dev.yokicore.app</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
</dict>
</plist>
PLIST
    LAUNCH="open \"$BUNDLE\""
    say "Installed: $BUNDLE"
else
    DESK="$HOME/.local/share/applications"
    mkdir -p "$DESK"
    cat > "$DESK/yokicore.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=YokiCore
Comment=Visual website builder
Exec=$APP/YokiCore
Terminal=false
Categories=Development;
ENTRY
    chmod +x "$DESK/yokicore.desktop"
    LAUNCH="nohup \"$APP/YokiCore\" >/dev/null 2>&1 &"
    say "Installed: $APP/YokiCore"
fi

say "Temporary files removed. Starting YokiCore..."
eval "$LAUNCH"
sleep 1
exit 0
#PS-BEGIN
$ErrorActionPreference = 'Stop'
$repo = 'OWNER/YokiCore'
$branch = 'main'
$raw = "https://raw.githubusercontent.com/$repo/$branch"
$app = if ($env:YOKICORE_APP) { $env:YOKICORE_APP } else { Join-Path $env:USERPROFILE 'YokiCore\app' }
$work = Join-Path $env:TEMP ('yokicore-' + [guid]::NewGuid().ToString('N'))

function Find-Gcc {
    $g = Get-Command gcc -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    $roots = @("$env:LOCALAPPDATA\Microsoft\WinGet\Packages", 'C:\mingw64\bin', 'C:\msys64\ucrt64\bin', 'C:\msys64\mingw64\bin', 'C:\ProgramData\mingw64\mingw64\bin')
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $f = Get-ChildItem -Path $r -Filter gcc.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($f) { return $f.FullName }
        }
    }
    return $null
}

try {
    $gcc = Find-Gcc
    if (-not $gcc) {
        Write-Host 'No C compiler found. Installing WinLibs with winget...'
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
            throw 'winget is not available. Install MinGW-w64 from https://winlibs.com and run this file again.'
        }
        winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e --accept-source-agreements --accept-package-agreements
        $gcc = Find-Gcc
        if (-not $gcc) { throw 'The compiler was installed but could not be found. Open a new window and run this file again.' }
    }

    New-Item -ItemType Directory -Path $work | Out-Null
    Write-Host 'Downloading source...'
    if ($env:YOKICORE_SRC) {
        Copy-Item (Join-Path $env:YOKICORE_SRC 'src\main.c') (Join-Path $work 'main.c')
        Copy-Item (Join-Path $env:YOKICORE_SRC 'src\ui.h') (Join-Path $work 'ui.h')
    } else {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest "$raw/src/main.c" -OutFile (Join-Path $work 'main.c') -UseBasicParsing
        Invoke-WebRequest "$raw/src/ui.h" -OutFile (Join-Path $work 'ui.h') -UseBasicParsing
    }

    Write-Host 'Building...'
    $env:PATH = (Split-Path $gcc) + ';' + $env:PATH
    Push-Location $work
    & $gcc -O2 -mwindows -o YokiCore.exe main.c -lws2_32 -lshell32
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw 'The build failed.' }

    New-Item -ItemType Directory -Force -Path $app | Out-Null
    Copy-Item (Join-Path $work 'YokiCore.exe') (Join-Path $app 'YokiCore.exe') -Force

    $desk = [Environment]::GetFolderPath('Desktop')
    $sh = New-Object -ComObject WScript.Shell
    $lnk = $sh.CreateShortcut((Join-Path $desk 'YokiCore.lnk'))
    $lnk.TargetPath = Join-Path $app 'YokiCore.exe'
    $lnk.WorkingDirectory = $app
    $lnk.Save()

    Write-Host "Installed: $app\YokiCore.exe"
    Write-Host 'A shortcut was added to your Desktop. Temporary files removed.'
    Start-Process (Join-Path $app 'YokiCore.exe')
} catch {
    Write-Host ('Setup failed: ' + $_.Exception.Message) -ForegroundColor Red
} finally {
    if (Test-Path $work) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
}
