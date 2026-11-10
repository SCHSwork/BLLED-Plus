## Print notifications (full edition)
Get a message on your phone or in a Discord channel when something happens on the printer. Set it up on the new **Notify** tab.

- **Where to:** a Discord webhook, the ntfy phone app (ntfy.sh or your own server), and/or any webhook as JSON (Home Assistant, Node-RED). Use any of them, or all three.
- **What:** print finished (with how long it took), failed or cancelled, the printer needs you (runout, clog, first-layer or spaghetti problem, AMS problem, a pause in the file, serious errors; not pauses you make yourself), print started, first layer done, and progress every 10 / 20 / 25 / 50 % with a progress bar (can be turned off).
- **Where each one goes:** pick which services get each kind, e.g. everything to ntfy but problems to Discord too.
- **Your own wording:** rewrite any notification with fill-ins like `{job}`, `{bar}`, `{percent}`, `{remaining}`, `{duration}` and `{hms}`.
- **Alerts for printer codes:** lines like `HMS_0700_* = AMS needs a look ({hms})` send your own message when that code shows up, even codes that don't pause the print. `off` silences a code.
- **Try it:** test buttons send a made-up example of each kind, to where it normally goes or to one service.
- Tapping a notification opens the controller's status page at blled.local.

The lite edition doesn't have room for notifications and is unchanged. To get them, move to the larger layout on the update page.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.48** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.48-lite.bin.ota** if you're not sure which layout you have.
