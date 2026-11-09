## No more flash after closing the door (for real this time)
With **Keep the printer's light in sync** on, closing the door could make the strip flash on once before going off. 2.8.38 fixed door bounces, but this flash had a different cause: the door turns the printer's light on when it opens and off when it closes, and the printer's status reports lag a second or two behind. Right after the door closed, a late report still said "light on" from when the door opened, and the strip followed it.

Now the controller ignores the printer's light reports for 4 seconds after it switches the light itself (door or camera). After that, the printer's actual state comes through as usual.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.39** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.39-lite.bin.ota** if you're not sure which layout you have.
