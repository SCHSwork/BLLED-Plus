## No more pulse when the door is closed hard
Closing the door firmly could make the lights flash once before going off. The door bounces off its switch, so the printer reports closed, open and closed again within a split second. The controller took the bounce as the door opening (lights on) and then as the "close twice" gesture (lights off).

Now an "open" that comes within a second after closing waits 0.4 seconds before doing anything. If the door closes again in that time, it was a bounce and is ignored. If the door really stays open, the lights come on as usual, just 0.4 seconds later. The Activity log notes each bounce and how long it lasted.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.38** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.38-lite.bin.ota** if you're not sure which layout you have.
