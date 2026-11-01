#!/usr/bin/env bash
# Builds the DLL and the installer engine (Windows .exe) on Linux/macOS. The Inno Setup wizard is built on Windows (setup/build_setup.bat).
set -e
cd "$(dirname "$0")"
mkdir -p installer/payload
cp ai/revived.ini installer/payload/revived.ini
python3 -m ziglang cc -target x86-windows-gnu -O2 -shared -DPUBLIC_BUILD -o installer/payload/dinput8.dll ai/dinput8.c ai/dinput8.def
(cd installer && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -H=windowsgui" -o ../setup/RevivedSands_Installer.exe .)
echo "Done: setup/RevivedSands_Installer.exe"
