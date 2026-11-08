## Watch printer messages (testing tool)
A new switch in **Settings → Device → Logging**, **Watch printer messages**, writes to the Activity log when a message from the printer contains a field it hasn't seen before, when the camera settings in the printer's reports change, or when the printer sends something other than a normal status report.

It's there to look for undocumented signals: first up, whether the printer says anything when someone opens the camera in Bambu Handy (so the lights could switch on for the camera). Leave it off unless you're helping test.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.28** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.28-lite.bin.ota** if you're not sure which layout you have.
