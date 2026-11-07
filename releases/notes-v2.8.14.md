## Fixed
- Checking GitHub for updates could run the controller out of memory while it was also talking to the printer ("Failed to grow buffer" in the log). It now pauses the printer connection for a few seconds while it checks.
- The **Activity** log skips lines that repeat (like the status line every few seconds), so it keeps much more history.
- Times in the log right after a restart showed UTC instead of your time zone. They now show the time since start-up until the time zone is loaded.

## How to update
On 2.8.13: open the update page and tap **Install 2.8.14** in the **Update from GitHub** card.
On 2.8.11 or 2.8.12: download **BLLC_V2.8.14.bin.ota** below and choose it on the update page.
