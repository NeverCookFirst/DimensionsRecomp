#!/bin/sh
# check-toypad.sh - is the LEGO Dimensions Toypad plugged in?
#
# For SteamOS / any Linux. No root, no dependencies: reads sysfs directly
# because SteamOS does not always ship lsusb.
#
#   ./check-toypad.sh        - verdict only
#   ./check-toypad.sh -a     - also list every USB device on every port
#
# Exit code: 0 = toypad found, 1 = not found.

TOYPAD_VID=0e6f
TOYPAD_PID=0241   # LEGO Dimensions Toypad (PS3/PS4/Wii U/X360 all report this)
                  # 0e6f:0129 is the Disney Infinity base, not ours.

ALL=0
[ "$1" = "-a" ] || [ "$1" = "--all" ] && ALL=1

found=0

# Pass 1: the port listing, so the verdict is not buried inside it.
if [ "$ALL" = 1 ]; then
    echo "USB-устройства на портах:"
    for dev in /sys/bus/usb/devices/*; do
        [ -f "$dev/idVendor" ] || continue
        printf '  %-12s %s:%s  %-7s %s %s\n' \
            "$(basename "$dev")" \
            "$(cat "$dev/idVendor" 2>/dev/null)" \
            "$(cat "$dev/idProduct" 2>/dev/null)" \
            "$(cat "$dev/speed" 2>/dev/null)M" \
            "$(cat "$dev/manufacturer" 2>/dev/null)" \
            "$(cat "$dev/product" 2>/dev/null)"
    done
    echo
fi

# Pass 2: the verdict.
for dev in /sys/bus/usb/devices/*; do
    [ -f "$dev/idVendor" ] || continue          # skip interfaces and root hubs
    vid=$(cat "$dev/idVendor" 2>/dev/null)
    pid=$(cat "$dev/idProduct" 2>/dev/null)
    name=$(cat "$dev/product" 2>/dev/null)
    maker=$(cat "$dev/manufacturer" 2>/dev/null)
    speed=$(cat "$dev/speed" 2>/dev/null)
    port=$(basename "$dev")

    is_toypad=0
    [ "$vid" = "$TOYPAD_VID" ] && [ "$pid" = "$TOYPAD_PID" ] && is_toypad=1
    # Fall back to the name, in case a revision ships a different PID.
    case "$name" in *[Tt]oy[Pp]ad*|*TOYPAD*) is_toypad=1 ;; esac
    [ "$is_toypad" = 1 ] || continue

    found=1
    echo "ТОЙПАД ПОДКЛЮЧЕН"
    echo "  ${vid}:${pid}  ${maker:-?} ${name:-?}"
    echo "  порт: $port  (${speed} Mbit/s)"

    # The app talks to it through hidraw, so a node we cannot open is
    # the same as no toypad at all - check it while we are here.
    hid=$(find "$dev" -maxdepth 4 -name 'hidraw[0-9]*' 2>/dev/null | head -n1)
    if [ -z "$hid" ]; then
        echo "  ВНИМАНИЕ: ядро не создало hidraw - драйвер usbhid не привязался"
    else
        node="/dev/$(basename "$hid")"
        if [ -r "$node" ] && [ -w "$node" ]; then
            echo "  $node - доступ есть, всё готово"
        else
            echo "  $node - НЕТ ДОСТУПА (нужно правило udev):"
            echo "    sudo steamos-readonly disable"
            echo "    echo 'SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"$TOYPAD_VID\", ATTRS{idProduct}==\"$TOYPAD_PID\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/99-lego-toypad.rules"
            echo "    sudo udevadm control --reload-rules && sudo udevadm trigger"
        fi
    fi
done

if [ "$found" = 0 ]; then
    echo "ТОЙПАДА НЕТ"
    echo "  Ни на одном порту нет ${TOYPAD_VID}:${TOYPAD_PID}."
    echo "  Запусти с -a, чтобы увидеть, что вообще видно на портах."
    exit 1
fi
