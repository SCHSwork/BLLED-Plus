## Fixed: running out of memory and crashing
Printer status messages are several KB each, and the buffer that holds them while they arrive (inherited from the original firmware) grew 128 bytes at a time for every message, then shrank back. Doing that several times a minute chopped the controller's memory into pieces too small to use. When that happened, it carried on with half a message ("Failed to grow buffer"), could glue a cut-off message onto the next one, and could lose the buffer entirely and crash.

Now the buffer is sized once per message (the size is known before it arrives) and kept between messages. If there really isn't enough memory for one message, that message is skipped cleanly (the next status report replaces it) and the Activity log says so once a minute.

## How to update
On 2.8.15 or newer: open the update page and tap **Install 2.8.24** in the **Update from GitHub** card (on 2.8.21–2.8.23 lite, tap **Try again** if it says the download failed).
By hand: use **BLLC_V2.8.24-lite.bin.ota** if you're not sure which layout you have.
