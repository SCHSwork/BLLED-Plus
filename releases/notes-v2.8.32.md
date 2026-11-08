## Clearer "Couldn't reach GitHub" messages
Checking for updates sometimes failed with "couldn't open a secure connection" and then worked on retry. That message was a guess: it covered every kind of connection failure. The page and the Activity log now say what actually went wrong:
- couldn't look up GitHub's address (the router or internet)
- no answer from GitHub in time (slow or dropped internet)
- not enough memory for a secure connection
- GitHub's certificate couldn't be checked
- or the exact error number from the secure connection

The check also tries 4 times instead of 3, waiting a bit longer between tries, so a short internet hiccup is less likely to stop it.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.32** in the **Update from GitHub** card. If it fails, tap **Check again**.
By hand: use **BLLC_V2.8.32-lite.bin.ota** if you're not sure which layout you have.
