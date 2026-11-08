## Steadier printer connection
The controller dropped its connection to the printer every minute or two, then reconnected (you'd see "Timeout (-4)" in the Activity log and the printer reconnect count climbing). The cause: the MQTT library hung up whenever the printer didn't answer one of its "are you there?" pings, even while the printer was sending status updates the whole time. The X1C doesn't always answer those pings.

BLLED Plus now counts any message from the printer as proof the connection is alive. A printer that really goes silent is still noticed and reconnected after 2½ minutes.

## Gentler updates
After a manual check on the update page, the printer connection now stays paused for a few seconds in case you tap Install, instead of reconnecting and dropping again right away. Quick reconnects in a row can make the X1C stop answering for a couple of minutes.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.22** in the **Update from GitHub** card. It picks the right edition by itself (on 2.8.20 and newer).
By hand: use **BLLC_V2.8.22-lite.bin.ota** if you're not sure which layout you have.
