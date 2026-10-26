#!/usr/bin/env bash
# Copyright (C) 2026 Vikotr Krapivin
#
# This file is part of emCORE.
#
# emCORE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# emCORE is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with emCORE. If not, see <http://www.gnu.org/licenses/>.

# setup.sh -- prepare a FRESH host to build & flash this standalone nano 5G emCORE bundle (README.md).
# This directory is self-contained: copy it to the new machine, run this once.
# Targets Debian/Ubuntu (apt). Installs the ARM cross toolchain, creates a
# local .venv (pyusb), and installs the udev rules for the iPod USB modes.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

need_sudo() {
    if [ "$(id -u)" -eq 0 ]; then return 0; fi
    if command -v sudo >/dev/null 2>&1; then sudo "$@"
    else echo "ERROR: need root to $* -- re-run as root or install sudo." >&2; exit 1; fi
}

echo "== 1) system packages (ARM cross toolchain + python + libusb + udev) =="
if command -v apt-get >/dev/null 2>&1; then
    need_sudo apt-get update
    need_sudo apt-get install -y \
        gcc-arm-none-eabi libnewlib-arm-none-eabi \
        python3 python3-venv python3-pip \
        libusb-1.0-0 udev
else
    echo "NOTE: no apt-get found -- install manually:"
    echo "      arm-none-eabi-gcc (newlib), python3, libusb-1.0-0."
fi

echo "== 2) python venv + deps =="
if [ ! -x .venv/bin/python ]; then python3 -m venv .venv; fi
.venv/bin/python -m pip install --upgrade pip >/dev/null
.venv/bin/python -m pip install -r requirements.txt

echo "== 3) udev rules (iPod USB modes: DFU 05ac:1231, emCORE ffff:e000) =="
need_sudo install -m 0644 udev/50-ipod-nano5g.rules /etc/udev/rules.d/50-ipod-nano5g.rules
need_sudo udevadm control --reload-rules 2>/dev/null || true
need_sudo udevadm trigger 2>/dev/null || true

echo "== 4) verify toolchain =="
command -v arm-none-eabi-gcc >/dev/null 2>&1 \
    || { echo "ERROR: arm-none-eabi-gcc not on PATH." >&2; exit 1; }
arm-none-eabi-gcc --version | head -1

echo
echo "== READY. Build: make | Output: emCORE/build/ipodnano5g/emcore.dfu | See README.md =="
