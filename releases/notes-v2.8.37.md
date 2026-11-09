## "Not enough memory for a secure connection" when updating from GitHub
Some controllers, mostly ones connected to printers that send large status messages, couldn't install from GitHub. They had enough free memory overall, but not in big enough pieces for the secure connection to GitHub. Two fixes:
- While the update runs, the controller now also frees the memory it keeps for printer messages, so there are bigger free pieces.
- If there still isn't enough, the controller restarts and does the update right after starting up, when memory is cleanest, before reconnecting to the printer. The update page says so and keeps going by itself. This only happens when the printer isn't printing, and only once per try.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.37** in the **Update from GitHub** card. If this is the error you've been getting, download the file instead this one time.
By hand: use **BLLC_V2.8.37-lite.bin.ota** if you're not sure which layout you have.
