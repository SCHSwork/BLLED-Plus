## No more false "access code refused" after the router hands out new addresses
After a Wi-Fi or power hiccup, routers sometimes give devices new addresses. If another printer ended up with the address your printer used to have, the controller logged in to the wrong printer with your access code. That printer refused it, so the strip blinked red and the status page said "access code refused", until a restart or a few minutes of waiting.

Now:
- If a different printer answers from the saved address, the controller doesn't log in there, so your access code is never sent to another printer.
- It searches right away, follows your printer to its new address, and connects immediately instead of waiting out the retry timer.
- The status page says "Your printer moved to …" instead of offering to switch to the other printer's serial number.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.51** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.51-lite.bin.ota** if you're not sure which layout you have.
