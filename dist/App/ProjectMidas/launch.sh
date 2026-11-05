#!/bin/sh
# OnionOS launcher for Project Midas.
#
# Installed as /mnt/SDCARD/App/ProjectMidas/launch.sh. Everything the game
# needs lives in this folder: the binary, the Miyoo SDL2 fork's shared
# objects (the mini video driver and its EGL/GLES), and Data/ (saves,
# settings, logs and the cart image).

cd "$(dirname "$0")" || exit 1

# MainUI starts us with a narrow library path, and SDL's mini video driver
# dlopens libEGL/libGLESv2 by bare name (not by path), so this folder has to
# be first. The rest is the stock OnionSDK search path.
export LD_LIBRARY_PATH="$PWD:/config/lib:/customer/lib:${LD_LIBRARY_PATH:-}"

mkdir -p Data/Logs

# Find the cart image the way the game does, but pick ONE file and pass it
# explicitly so the log says which image was used.
ROM=""
for f in Data/*.z64 Data/*.v64 Data/*.n64 Data/*.Z64 Data/*.V64 Data/*.N64; do
    if [ -f "$f" ]; then
        ROM="$f"
        break
    fi
done

# MainUI owns the panel and the audio device. Park it for the run and wake it
# however we exit, including on error or Ctrl-C from a debug shell.
MAINUI_PID=$(pidof MainUI 2>/dev/null)
[ -n "$MAINUI_PID" ] && kill -STOP "$MAINUI_PID" 2>/dev/null
trap '[ -n "$MAINUI_PID" ] && kill -CONT "$MAINUI_PID" 2>/dev/null' EXIT INT TERM HUP

# The console is not visible on the device, so the run log is the only
# evidence a launch attempt leaves behind.
LOG=Data/Logs/launch.log
{
    echo "=== $(date) ==="
    echo "rom: ${ROM:-<none, letting the engine search Data/>}"
    echo "ld_library_path: $LD_LIBRARY_PATH"
    uname -a
} >> "$LOG" 2>&1

if [ -n "$ROM" ]; then
    ./ProjectMidas --rom-file "$ROM" >> "$LOG" 2>&1
else
    ./ProjectMidas >> "$LOG" 2>&1
fi

STATUS=$?
echo "exit status: $STATUS" >> "$LOG" 2>&1
exit $STATUS
