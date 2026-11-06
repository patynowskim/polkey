#!/usr/bin/env bash
# Flash the FIDO2 app to the PolKey BL616 (app at 0x20000; the Sipeed bootloader at 0x0 is untouched).
#
# Put the board in UPDATE mode first: unplug it, hold the UPDATE button (behind the HDMI
# connector) while plugging USB back in; it appears as "Bouffalo CDC DEMO" /dev/ttyACM0.
# Then run:  ./flash_fidokey.sh [port]
# Afterwards unplug and plug the board in normally.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/../fidokey"
SDK="$APP/bouffalo_sdk"
T="$SDK/tools/bflb_tools/bouffalo_flash_cube/BLFlashCommand-ubuntu"
PORT="${1:-/dev/ttyACM0}"

[ -x "$T" ] || { echo "flash tool not found: $T (clone bouffalo_sdk into $APP)"; exit 1; }
[ -c "$PORT" ] || { echo "$PORT not found - is the board in UPDATE mode?"; exit 1; }

cd "$APP"
exec "$T" --interface=uart --baudrate=2000000 --port="$PORT" --chipname=bl616 --cpu_id= --config=flash_app_only.ini
