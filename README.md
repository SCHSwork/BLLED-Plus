# BLLED Plus

Unofficial firmware for the **BLLED controller** on Bambu Lab printers. It started as a fork of [DutchDevelop's BLLEDController](https://github.com/DutchDevelop/BLLEDController) 2.3.0 and adds smarter lighting, a new web interface and a more stable connection. Tested on an X1C.

> Not made or endorsed by DutchDevelop or Bambu Lab. Same license as the original (CC BY-NC-SA 4.0): free for personal use, not for sale.

![BLLED Plus 2.5](docs/screenshots/BLLED-Plus-2.5.png)

## Install

### Update a controller that's already set up

Works from the official BLLED firmware or an older BLLED Plus. Your settings carry over.

1. Download the newest `BLLC_V*.bin.ota` from [Releases](../../releases). On a phone, save it to your Downloads.
2. On the same Wi-Fi as the controller, open **http://blled.local/fwupdate**, or open **http://blled.local** and tap **Update firmware** at the bottom of the settings page. (From 2.7.3 on, `/update` works too.)
   If blled.local doesn't load, use the controller's number instead (for example `http://192.168.1.42/fwupdate`). Your router's app lists it under connected devices.
3. Choose the `.bin.ota` file and upload it. Don't unplug anything until it says it's done.
4. Wait about 30 seconds for the restart, then open **http://blled.local/status**. The firmware version is at the bottom.

**Going back:** upload an official BLLED firmware file the same way.

### Set up a brand-new controller

1. Flash the official firmware with the [BLLED web flasher](https://dutchdevelop.github.io/blledsetup/) (Chrome or Edge on a computer, controller plugged in over USB).
2. On your phone, join the Wi-Fi hotspot **BLLED_AP**. The setup page should open by itself. If it doesn't, go to **http://192.168.4.1**.
3. Choose your home Wi-Fi and enter its password.
4. Enter your printer's details, all found on the printer's screen:
   - **IP address** and **access code**: Settings, then WLAN / Network
   - **Serial number**: Settings, then Device
5. Save. The controller restarts and joins your Wi-Fi.
6. Install BLLED Plus with the update steps above.

### Settings

- **http://blled.local/** - lights, colors, effects and everything else
- **http://blled.local/status** - what the strip is doing, print progress, temperatures and connection health
- **http://blled.local/backuprestore** - save your settings to a file and restore them

Each setting has a **Preview** button that shows it on the real strip without saving.

### Phones

- If the page won't open in Chrome on Android, try Samsung Internet or Firefox. Some browsers force `https://`, and the controller only speaks `http://`.
- Give the controller a fixed address in your router (often called *address reservation* or *DHCP reservation*), so a router restart never moves it.

### Troubleshooting

| What you see | What to do |
|---|---|
| Strip blinks dim red | The printer refused the access code. Check it on the printer screen (it changes if LAN settings change) and update it under Wi-Fi and printer connection. |
| Status page says the serial belongs to a different printer | Tap the button on the status page to use the printer that's actually at that address. |
| "Connected, but the printer isn't sending anything" | The serial number is for a different printer. Re-enter it. |
| Can't reach the controller | Try `http://blled.local`. Check that your phone is on the same Wi-Fi, not a guest network. Unplugging the controller for 10 seconds restarts it. |

The log is at **http://blled.local/webserial** if you need to see what the controller is doing.

## What it does

**While printing**
- **Filament color**: pulses in the incoming spool's color during AMS swaps and loading; while printing it stays your normal color with a short filament-color swell about once a minute.
- **First-layer spotlight**: full-brightness white for layer 1. Every lidar step (bed scan, extrusion calibration, first-layer inspection) keeps its own dark setting.
- **Almost-done heads-up**: shifts toward the finished color in the last minutes.
- **Progress glow** (optional) and **hold steady during timelapses**.

**Before and after**
- **Heat-up glow**: blue, purple, red, orange as the bed or nozzle heats, at print start or from the printer screen.
- **Safe to remove**: holds the finished color until the bed has cooled.
- **Finish celebration**: a short rainbow when a print ends.

**Problems, each with its own color**
- Waiting for you (amber, breathing): the print paused on its own. Pauses you make keep the pause color.
- First layer or spaghetti (pink), nozzle clog (yellow), AMS problem (purple), filament runout and faults (red).

**Around the house**
- **Door work light**, and lights the door turned on go back off when it closes.
- **Dims when idle** before turning off, and **quiet hours** at night.
- **Printer light sync**: the chamber light follows the strip outside prints.
- **Preview** any color or effect on the real strip before saving.

**Web interface and connection**
- New settings and status pages: print progress, time left, "done around", temperatures, connection health and the controller's address.
- Non-blocking LEDs, MQTT back-off and watchdog, Wi-Fi recovery, and a settings-page self-check that restarts a frozen web server (never mid-print).
- The controller shows up as "BLLED" in your router and at `blled.local`.

## Screenshots

| Status | Lights | Alerts | Effects |
|---|---|---|---|
| ![Status](docs/screenshots/1-status.png) | ![Lights](docs/screenshots/3-lights.png) | ![Alerts](docs/screenshots/4-alerts.png) | ![Effects](docs/screenshots/5-effects.png) |

## Building from source

- **Windows:** install PlatformIO, then run `build-firmware.bat`. `flash-firmware.bat` flashes over USB.
- **Linux:** `tools/build-ota-linux.sh <toolchain-dir>` builds the `.bin.ota` with arduino-cli.
- The web pages in `src/www` are compressed into `.h` files by `pre_build.py` before every build. Those generated files aren't checked in.
- BLLED Plus uses a larger firmware space (`min_spiffs.csv`). A controller on the original layout still takes the `.bin.ota` update. To move one to the larger layout over USB without losing settings, run `migrate-layout.bat` (it backs up the settings first).

---

## License and credits

### License

BLLED Plus, like the original BL Led Controller, is released under Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0) license. See [LICENSE](LICENSE). Free to share and change for non-commercial use, with credit, under the same license.

### Credits

**BLLED Plus**
- **[Lincoln (SCHSwork)](https://github.com/SCHSwork)**: creator of BLLED Plus. New lighting features, web interface, phone app, connection fixes and testing on the X1C.

**Original BLLEDController**, which BLLED Plus is built on
- **[DutchDeveloper](https://dutchdevelop.com/)**: creator and lead programmer
- **[Modbot](https://github.com/Modbot)**: tester for X1C, P1P & P1S
- **[xps3riments](https://github.com/xps3riments)**: inspiration for the foundation of the code
- **[longrackslabs](https://github.com/longrackslabs)**: build process, documentation, developer & community support
- **[SoftWareCrash](https://github.com/softwarecrash)**: smaller changes
