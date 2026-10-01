#!/bin/bash
# port-power-check: show each USB port's power limit and how many times macOS has cut its power.
#
# A port that can't supply what the device draws trips its over-current protection: the device
# disconnects, comes back, and often disconnects again. macOS counts each cut in the port's
# "port-statistics" (kPortStatOverCurrentCount). The counters reset when the Mac restarts.
#
# Usage:
#   ./port-power-check.sh          ports with a device plugged in, or with any power cut
#   ./port-power-check.sh --all    every USB port
#
# Read-only: it only reads the I/O Registry (ioreg). No sudo needed.

ALL=0
[ "$1" = "--all" ] && ALL=1

ioreg -l -w0 2>/dev/null | LC_ALL=C awk -v all="$ALL" '
function flush() {
    if (loc != "" && (all || dev != "" || cuts + 0 > 0))
        printf "0x%-10s %6s %9s %11s  %s\n", loc, (limit == "" ? "?" : limit "mA"), conn, cuts, dev
    loc = ""; limit = ""; conn = ""; cuts = ""; dev = ""
}
BEGIN { printf "%-12s %6s %9s %11s  %s\n", "LOCATION", "LIMIT", "CONNECTS", "POWER-CUTS", "DEVICE" }
# A port entry, e.g. "+-o AppleUSB20HubPort@20230000  <class AppleUSB20HubPort, ...>"
/\+-o [A-Za-z0-9]*Port@[0-9a-f]+  </ {
    flush()
    match($0, /Port@[0-9a-f]+/); loc = substr($0, RSTART + 5, RLENGTH - 5)
    next
}
# The device plugged into that port carries the same location, e.g. "+-o MacBook Air SuperDrive@20230000"
/\+-o .+@[0-9a-f]+  <class IOUSB(Host)?Device[,>]/ {
    if (loc != "" && dev == "" && index($0, "@" loc "  <class") > 0) {
        line = $0; sub(/^.*\+-o /, "", line); sub(/@[0-9a-f]+  <class.*$/, "", line); dev = line
    }
    next
}
/"kUSBWakePortCurrentLimit" = / { if (loc != "" && limit == "") { limit = $NF } }
/"port-statistics" = / {
    if (loc != "") {
        if (match($0, /"kPortStatConnectCount"=[0-9]+/)) conn = substr($0, RSTART + 24, RLENGTH - 24)
        if (match($0, /"kPortStatOverCurrentCount"=[0-9]+/)) cuts = substr($0, RSTART + 28, RLENGTH - 28)
    }
}
END { flush() }
'
