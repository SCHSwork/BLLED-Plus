## Move to the larger layout without a cable (lite edition)
Controllers on the original BLLED storage layout run the lite edition. The update page on those controllers now has a **Move to the larger layout** button. It installs the full edition and gives the controller room for future features, with no USB cable needed. Your settings are kept.

How it works: it saves your settings somewhere safe on the chip, downloads the full edition from GitHub and checks its signature, then switches the chip to the larger layout and restarts into it.

**The one risk:** for about 0.05 seconds the controller rewrites the table that says how its storage is divided. If the power goes out in exactly that moment, it won't start until it's re-flashed over USB. Losing power at any other point is fine: it starts on the lite edition and you can try again. Keep it plugged in for the 2 minutes it takes. Moving is optional; the lite edition keeps getting updates either way.

## Also
- The lite edition no longer has the live web log page (it needed the room). The **Activity** log on the status page shows the same information, and the old log link opens it.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.21** in the **Update from GitHub** card. It picks the right edition by itself.
By hand: use **BLLC_V2.8.21-lite.bin.ota** if you're not sure which layout you have; it works on every controller.
