Everything since v2.8.3.

## Update straight from GitHub
- The update page has a new **Update from GitHub** card: it tells you when a new version is out and installs it with one tap. The controller downloads the file itself, so there's nothing to save on your phone.
- **Install new versions automatically** (Settings → Updates): the controller checks for new releases every 6 hours and installs them only while the printer isn't printing. You can limit it to certain hours. Off by default.
- Safe by design: the download is checked against GitHub's security certificate, a damaged or cut-off download changes nothing, and if a new version doesn't start properly the controller goes back to the old one and won't try that release again on its own.
- The Status and Lights pages show a notice when a newer version is available.

## How to update to this version
This is the last time you need to install by hand: download **BLLC_V2.8.6.bin.ota** below and upload it on the update page (use the controller's IP address, e.g. `http://192.168.1.50/fwupdate`). From now on, updates come from the **Update from GitHub** card.
