#!/usr/bin/env bash
# What the compositor actually did to the window, asked rather than eyeballed.
#
# Hyprland will answer questions about a window directly - where it is, how big
# it is, whether it is full screen - and that is worth more than a screenshot,
# because a screenshot cannot tell the difference between "the client asked for
# this size" and "the compositor imposed it". Screenshots are taken anyway, for
# the things only an eye catches.
#
# Run it over ssh from the host:  ssh picoview-vm 'bash -s' < vm-test.sh
set -uo pipefail

export XDG_RUNTIME_DIR=/run/user/$(id -u)
sig=$(ls -t "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1 || true)
if [[ -z ${sig:-} ]]; then
    echo "FAIL: Hyprland is not running - nothing to ask"
    echo "--- what the session said ---"
    tail -40 "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log 2>/dev/null || true
    journalctl --user -n 40 --no-pager 2>/dev/null || true
    exit 1
fi
export HYPRLAND_INSTANCE_SIGNATURE=$sig
export WAYLAND_DISPLAY=wayland-1

out=~/out
mkdir -p "$out"
rm -f "$out"/*.png "$out"/*.txt

note() { printf '\n=== %s\n' "$*"; }
shot() { grim "$out/$1.png" 2>/dev/null && echo "  shot $1.png"; }

note "compositor"
hyprctl version | head -3
hyprctl monitors | grep -E "^Monitor|^\s+[0-9]+x[0-9]+" | head -4

note "starting picoview"
pkill -x picoview 2>/dev/null; sleep 1
picoview ~/pics > "$out/picoview.log" 2>&1 &
sleep 3

win() { hyprctl clients -j | python3 -c "
import json,sys
for c in json.load(sys.stdin):
    if c.get('class') == 'picoview':
        print('title=%s at=%s size=%s floating=%s fullscreen=%s monitor=%s'
              % (c.get('title'), c.get('at'), c.get('size'),
                 c.get('floating'), c.get('fullscreen'), c.get('monitor')))
        break
else:
    print('NO WINDOW')
"; }

note "window as the compositor sees it"
win | tee "$out/win-normal.txt"
shot 1-normal

note "next picture (Right)"
hyprctl dispatch sendshortcut ,Right,class:picoview >/dev/null
sleep 2
win
shot 2-next

note "full screen (the compositor's own, not ours)"
hyprctl dispatch fullscreen 0 >/dev/null
sleep 2
win | tee "$out/win-fullscreen.txt"
shot 3-fullscreen
hyprctl dispatch fullscreen 0 >/dev/null
sleep 1

note "made narrow and tall, to see a resize land"
hyprctl dispatch setfloating active >/dev/null
hyprctl dispatch resizeactive exact 520 860 >/dev/null
sleep 2
win | tee "$out/win-resized.txt"
shot 4-resized

note "a track with no cover, drawn from its name"
pkill -x picoview; sleep 1
picoview ~/pics/Jenny.mp3 >> "$out/picoview.log" 2>&1 &
sleep 3
win
shot 5-cover

note "still alive?"
if pgrep -x picoview > /dev/null; then echo "  yes"; else echo "  NO - it exited, see picoview.log"; fi
pkill -x picoview 2>/dev/null

note "anything it said"
cat "$out/picoview.log"
echo
echo "screenshots in $out"
ls -1 "$out"
