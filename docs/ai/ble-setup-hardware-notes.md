# BLE setup and hardware notes (historical)

The BLE mount described here was replaced by the WiFi hub, see
[`wifi-hub-and-accounts.md`](wifi-hub-and-accounts.md). What stays useful are the
hardware and pairing traps, which cost a lot to rediscover.

Board: Waveshare ESP32-S3-Touch-AMOLED-2.16. Firmware built and flashed from WSL;
the Python daemon ran on Windows, because WSL2 does not expose the host's
Bluetooth.

## Four traps

1. **Single-owner lock.** The board stores the address of the PC it is bound to
   in NVS and only advertises to that one, so it is invisible to any scan while
   Windows shows no pairing. Only the serial output reveals it
   (`BLE: owner loaded = ...`). The official gesture is a long press on power;
   the software equivalent is erasing NVS:
   `esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_region 0x9000 0x5000`.
2. **Pair from Python, not PowerShell.** PowerShell 5.1 does not marshal
   scriptblocks into WinRT callbacks: `PairingRequested` never fires and
   `PairAsync` returns `RejectedByHandler` / `Failed`, on all four protection
   levels. `bleak` does it in three lines:
   `BleakClient(addr).pair(protection_level=2)`. Note that `CanPair` is only
   `True` on the AssociationEndpoint object, never on the BluetoothLEDevice.
   Without pairing the symptom is misleading: the daemon logs a perfect
   `Sending: {...,"ok":true}` while the screen stays on "listening / no data",
   because the firmware drops every write on an unencrypted link
   (`BLE: dropping RX write from unencrypted link`). A clean daemon log proves
   nothing; only the screen does.
3. **A stale Windows `.credentials.json` shadows the WSL one** (see the WiFi
   notes). `CLAUDE_CREDENTIALS_PATH` takes precedence over everything.
4. **User-level environment variables are not inherited by a process started from
   WSL.** A `powershell.exe` launched from WSL must have them set explicitly
   before `Start-Process`. The HKCU Run autostart does inherit them at login.

Flashing from WSL: `usbipd-win` attaches the USB busid to WSL, then
`pio run -e waveshare_amoled_216 -t upload`. A `usbipd detach` reboots the board.

## Battery

The board has an MX1.25 connector; charging is handled by the AXP2101. A 3.7 V
1000 mAh cell is sometimes supplied. Expect about 2-3 h at full brightness
(extrapolated from the 1.8 inch model). **Check polarity with a multimeter before
connecting a third-party battery**: the 1.25 mm pitch is standard, the wire order
is not. The TF slot exists but is unused.

## Forks worth knowing

Upstream declined WiFi (PR #40 dormant since June, issue #110 closed as not
planned), so the pieces come from forks:

- `alasdaircs/clawdmeter-wifi`, branch `wifi-native`: the only one tested on this
  board, but it stores the OAuth token in NVS on the device.
- `JacquesDelaigue/Clawdmeter`, branch `feat/wifi-transport`: keeps the token on
  the PC and falls back to HTTP on the LAN.
- `egorshar/Clawdmeter`, branch `dual-account`: the `slots` abstraction reused
  here. Switching is a touch swipe because no button is free (PWR already handles
  brightness on the Usage screen; the upstream README is out of date on this).
