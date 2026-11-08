## More reliable updates and layout moves
- **Signatures come with the release info.** Each release's signatures are now in its release notes (hidden), so the controller gets them along with the version check instead of downloading a separate file. That was the step that sometimes timed out ("Couldn't download the signature"), and updates are about 10 seconds faster.
- **Retries:** checking GitHub, getting a signature and starting the download each try up to 3 times before giving up, instead of stopping at the first Wi-Fi or memory hiccup.
- **Honest errors:** "connection refused" really meant the controller couldn't open a secure connection itself (usually low memory). The message now says that, with the free memory.
- **The update page recovers on its own:** if the controller stops answering for a moment, the page says "Lost contact... trying again" and picks up where it was, instead of freezing on "Checking...".
- Moving to the larger layout now shows the real reason if it stops.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.25** in the **Update from GitHub** card. If it fails on 2.8.21–2.8.24 lite, install the file by hand once.
By hand: use **BLLC_V2.8.25-lite.bin.ota** if you're not sure which layout you have.
