## Fixed
- The lite edition in 2.8.21 and 2.8.22 kept about 30 KB of memory reserved all the time for moving to the larger layout, even when nothing was moving. With less memory free, updating from GitHub could fail ("Download failed: connection refused") and the controller could struggle to read printer messages ("Failed to grow buffer"). That memory is now only borrowed while a move runs.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.23** in the **Update from GitHub** card. On 2.8.21 or 2.8.22 lite, if it says "Download failed", tap **Try again**, or install the file by hand once.
By hand: use **BLLC_V2.8.23-lite.bin.ota** if you're not sure which layout you have.
