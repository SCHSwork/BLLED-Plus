## Important fix for "Move to the larger layout" (lite edition)
**If you're on 2.8.21–2.8.26 lite, don't use "Move to the larger layout" until you've updated to this version.**

In those versions, the last step of the move wrote the storage table with interrupts switched off. On real hardware, erasing flash can take long enough that the chip's safety watchdog resets it partway through that step. If that happens after the old table is erased, the controller needs a USB re-flash. This version writes the table with the chip's normal flash routines, the same ones used for every other step of the move, which keep the watchdog satisfied while the flash is busy.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.27** in the **Update from GitHub** card.
On older versions: download **BLLC_V2.8.27-lite.bin.ota** (or the full one if you're on the larger layout) and choose it on the update page.
