## Ready for Home Assistant
This release prepares the controller for the new **BLLED Plus for Home Assistant** integration (coming soon at SCHSwork/ha-blled-plus):
- The full edition now announces itself on the network, so Home Assistant finds it by itself. Lite has no room for that, so there you enter the controller's address once in Home Assistant.
- The status now includes the controller's network ID, so Home Assistant can tell several controllers apart.

Nothing else changes. Without Home Assistant, the controller works exactly as before.

## How to update
On 2.8.25 or newer: open the update page and tap **Install 2.8.50** in the **Update from GitHub** card.
By hand: use **BLLC_V2.8.50-lite.bin.ota** if you're not sure which layout you have.
