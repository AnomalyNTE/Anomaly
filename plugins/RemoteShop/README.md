# Remote Shop / 随身商店

Browse NTE merchants and purchase products from an Anomaly window. The server still
settles prices, currency, stock and order acceptance.

- Merchant and branch cards, product icons, and global keyword search.
- One card per actual item, display item, currency and price; choose a branch from
  the source label or product details. Confirmed sold-out offers are hidden.
- Product and shop favorites, saved quantities, and restart restoration. Older
  source-based favorites remain readable and appear as merged product cards.
- A separate batch tab with per-product quantities and removal. Each entry keeps
  its real shop/offer identity. Orders run sequentially and wait for receipts;
  timeout, partial confirmation or player changes stop subsequent orders.
- English fallback text and a Simplified Chinese localization catalog.

Compatibility data is loaded through `anomaly.profile` from
`profile/compatibility.profile.json`. Reflected properties and function parameters
are checked before use; the native actor dispatch and FText fallback also require
the corresponding host feature gates. An unsupported profile disables loading.

Favorites use `anomaly.config` at
`Anomaly/config/plugins/b1ank.remote-shop/config-favorites.json`. Changes are queued
from the UI and saved on an `anomaly.scheduler` Worker; a failed save restores the
last durable value. Replacing the plugin package preserves this configuration.
The same file remembers the active page, search filters, quantity and preferred
sources. In-flight purchases and the batch selection are temporary session state.

Icons are exported by the game into the process-specific temporary
`AnomalyRemoteShopIcons` directory, normalized with Windows Imaging Component, and
cached as a bounded set of host-owned textures. File reads and image decoding run
on a Worker; drawing only requests cached resources. The small selection fallback font
is an original generated asset; it does not contain game artwork.

Build with the repository presets:

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --target anomaly_builtin_remote_shop --parallel
pwsh -NoProfile -File tools/check_plugin_i18n.ps1 -Plugin RemoteShop
```
