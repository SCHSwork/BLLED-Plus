## Printer errors in plain English
When the printer reports a problem, the status page now says what it means instead of only showing a code. For example: "The nozzle temperature is abnormal; the heater may have a short circuit. (HMS_0300_0200_0001_0001)", with a link to that error on the Bambu wiki.
- Covers about 4,900 printer health (HMS) codes and 940 print errors, using Bambu's own descriptions (the ones Bambu Studio shows). H2 printers get the H2 wording, such as "the right nozzle".
- When the printer pauses or stops a print by itself, the status page now says why.
- The Activity log shows the meaning under each line with an error code.

On the full edition, the descriptions are built into the controller and work without internet. On the lite edition, there isn't room, so the status page looks them up on GitHub (your phone or computer needs internet for that).

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.36** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.36-lite.bin.ota** if you're not sure which layout you have.
