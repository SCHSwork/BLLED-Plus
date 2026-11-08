## Settings survive power cuts
Saving settings used to erase the old file first and then write the new one. If the power went out in that moment, the controller came back with no settings at all, Wi-Fi included, and started the setup hotspot as if it were new. (The original BLLED firmware does the same, which is a likely cause of "it forgot my Wi-Fi after a power cut".)

Now the new settings are written to a separate file and only swapped in once they're complete, and the previous version is kept. If a save ever gets cut off, the controller starts with the last good settings.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.17** in the **Update from GitHub** card, or let automatic updates handle it.
On older versions: download **BLLC_V2.8.17.bin.ota** below and choose it on the update page.
