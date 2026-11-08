## Settings survive power cuts
After a power cut or a flicker, the controller sometimes starts while the power is still settling, and reading the settings can fail once. The original firmware then wrote the default settings over the file, or deleted it, so one bad start wiped everything, including Wi-Fi and the printer connection. BLLED Plus inherited that code. Now:
- **A settings file that can't be read is never overwritten or deleted.** The controller tries again a few times, then restarts (up to 3 times in a row) to read it fresh. If it still can't, it runs on defaults for now and leaves the file alone, so the next good start has everything back.
- **A second copy of your settings is kept** in a separate part of the chip's memory. If the file is damaged, or the storage ever has to be reformatted, the settings come back from that copy.
- **The storage is given a few tries to start** before it's ever reformatted.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.35** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.35-lite.bin.ota** if you're not sure which layout you have.
