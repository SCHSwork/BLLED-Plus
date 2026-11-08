## Fixed
- **Update from GitHub** refused correctly signed releases on 2.8.11–2.8.14: the controller picked up one stray extra byte when downloading the signature, so the check failed. It now reads exactly the signature's length.

## How to update
**On 2.8.11–2.8.14, update by hand this once:** download **BLLC_V2.8.15.bin.ota** below and choose it on the update page. From 2.8.15 on, **Update from GitHub** installs signed releases.
On 2.8.6–2.8.10: tap **Install 2.8.15** in the **Update from GitHub** card.
