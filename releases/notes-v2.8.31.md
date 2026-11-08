## Lights on for the camera
New in **Settings → Lights → When nothing is happening → Lights on for the camera**: when someone opens the printer's camera in Bambu Handy, the printer's light turns on (and the strip with it), so you can actually see the print. It turns back off a set time after the camera was last opened (5 minutes by default). It's off by default.

How it works: when the camera opens, Bambu Handy asks the printer to get the live view ready, and the printer announces that on its local connection. We found that with the **Watch printer messages** tool, so thanks to everyone who tested. The printer doesn't announce when the camera closes, so the light goes off on a timer. During a print the printer runs its own light, so this does nothing while printing.

## Also
- **Watch printer messages** no longer writes the camera's session password to the Activity log.
- Includes 2.8.30 (back to the home page after an update) and 2.8.29 (crash reports, the printer connection is closed properly before restarts).

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.31** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.31-lite.bin.ota** if you're not sure which layout you have.
