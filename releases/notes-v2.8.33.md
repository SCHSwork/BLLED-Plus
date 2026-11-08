## Camera light timer in seconds
**Lights on for the camera** now takes seconds instead of minutes (5 seconds to 1 hour, 60 seconds by default), since most people only glance at the camera. If you set it on 2.8.31, your time carries over.

## Watch printer messages: clearer camera changes
When the printer's camera settings change, the Activity log now shows just the setting that changed and its old and new value (for example `ipcam.bs_state: 0 -> 1`), instead of a cut-off copy of all of them. This is to look for a sign that the camera has been closed again, so the light could stay on exactly as long as someone is watching.

Watch printer messages is now in the full edition only, to keep the lite edition small enough for the original layout.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.33** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.33-lite.bin.ota** if you're not sure which layout you have.
