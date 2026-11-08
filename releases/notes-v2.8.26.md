## Fixed: moving to the larger layout from the second firmware slot
Controllers running from their second firmware slot first copy themselves to the first slot when moving to the larger layout. That copy could stop with "Not enough memory to copy" if a big status message from the printer was being read at the same moment. The move now pauses the printer connection from the start (it comes back afterwards) and the copy uses a smaller buffer that waits a moment if memory is briefly short.

## How to update
On 2.8.25: open the update page and tap **Install 2.8.26** in the **Update from GitHub** card.
On older versions: download **BLLC_V2.8.26-lite.bin.ota** (or the full one if you're on the larger layout) and choose it on the update page.
