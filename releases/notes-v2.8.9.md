## Fixed
- **Update from GitHub** could fail with "Couldn't start installing: No Error". The controller ran out of memory because the printer connection stayed open during the download. It now pauses the printer connection while downloading (it comes back after) and sets up the install before the download starts.

## How to update
On 2.8.6–2.8.8 the GitHub button has this bug, so update by hand one more time: download **BLLC_V2.8.9.bin.ota** below and choose it on the update page. After that, the **Update from GitHub** card handles updates.
