## Fixed
- On 2.8.11 and 2.8.12, **Update from GitHub** could refuse a correctly signed release ("The file isn't signed by BLLED Plus"). The signature is now checked against the firmware exactly as it was saved on the controller.
- A release that fails the signature check is no longer downloaded again and again by automatic updates.
- If the check ever fails, the message includes details that show why.

## How to update
**On 2.8.11 or 2.8.12, update by hand this once:** download **BLLC_V2.8.13.bin.ota** below and choose it on the update page.
On 2.8.6–2.8.10: tap **Install 2.8.13** in the **Update from GitHub** card.
