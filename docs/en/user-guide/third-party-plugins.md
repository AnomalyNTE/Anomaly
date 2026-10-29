# Third-party plugins

Anomaly can download, update and uninstall community plugins straight from a plugin source. You do not normally need to hunt for DLLs by hand or copy files into the game directory.

## Take a look before you install

Third-party plugins run as native DLLs inside the game process, exactly like built-in plugins. The capabilities in a manifest only limit which Anomaly services the plugin may call; they are not a Windows security sandbox.

> [!WARNING]
> Plugin sources are maintained by the community, and Anomaly does not review plugins on your behalf. Neither the current plugin catalog nor the ZIP packages carry a publisher signature. HTTPS, package structure checks and manifest validation stop damaged or misplaced files, but they cannot prove that code is safe. Add only plugin sources you trust, and install only plugins whose origin you know.

Before installing, confirm three things:

- Whether the author and the source repository are trustworthy, and whether they are still maintained recently.
- Whether the plugin description states its purpose clearly, and whether the capabilities the manifest requests match that purpose.
- Whether the plugin supports the current Anomaly Plugin API and `nte`.

## Download and enable a plugin

1. Once in the game, press `Insert` to open Anomaly.
2. Open **Plugins > Available**. The page lists the plugins from the enabled plugin sources; incompatible entries show the reason and disable the install button.
3. Find the plugin and click **Install**, then wait for the status to turn to success.
4. Switch to **Installed**, select the plugin you just installed and enable it.

New plugins are not enabled automatically by default. That way you can read the description, status and required permissions before you decide to run them.

If the install button is grayed out, the game or Plugin API version the plugin declares usually does not match the current Runtime. The page shows the specific reason.

> [!NOTE]
> Installing a plugin does not download its required dependencies along with it. If the status reports missing dependencies after you enable it, go back to **Available** and install the matching dependencies too.

## Update a plugin

After a plugin source publishes a higher version, the plugin appears under **Plugins > Updates**:

1. Open **Updates** and confirm the version and the source.
2. Click **Update** and wait for the download and installation to finish.
3. Go back to **Installed** to check the run status; reload manually when needed.

During an update the Runtime validates the new package first and only then replaces the old directory. If the download, extraction or validation fails, the existing plugin is not overwritten by half of a new package.

## Uninstall a plugin

Plugins that you installed from a plugin source and that still appear in the **Available** catalog can be uninstalled from the action menu or the detail page on the **Installed** page. Uninstalling deletes the plugin package but keeps the settings under `Anomaly\config\plugins\<plugin ID>` and the data under `Anomaly\state\plugins\<plugin ID>`, so they remain usable after you reinstall.

Plugins you copied by hand, or plugins that have been removed from their plugin source, must be deleted manually:

1. Exit the game.
2. Delete the matching `Anomaly\plugins\<plugin directory>`.
3. To clear the settings and data as well, also delete `Anomaly\config\plugins\<plugin ID>` and `Anomaly\state\plugins\<plugin ID>`.

If you are unsure which plugin a directory belongs to, open the `manifest.json` inside it and check `id` and `name` instead of guessing from the folder name.

## Manage plugin sources

Open **Plugins > Third-party plugins** to manage plugin sources:

1. Turn on the "Enable third-party plugins" master switch.
2. Click "Add source" and enter the `pluginmaster.json` HTTPS URL the source provides.
3. Select the sources you want to enable and click "Apply".
4. Go back to **Available** and click the refresh button in the top-right corner.

The runtime package ships with one plugin source already enabled. The actual URL is stored in `Anomaly\plugin-repositories.json`; do not rely on a fixed link taken from the documentation.

You can disable each plugin source on its own, and the URL is kept after you disable it. When several plugin sources carry the same plugin ID, the catalog shows only the entry with the highest version, so do not add sources of unknown origin.

Production plugin sources and download URLs must use HTTPS. `file://` is reserved for plugin authors doing local testing; normal use does not need it. For the full configuration file format, see [Configuration reference](configuration.md).

## What Anomaly checks

Before installation it checks:

- Whether the plugin source and the download URLs follow the transport rules.
- Whether the ZIP contains out-of-bounds paths, duplicate targets or an unusual size.
- Whether the ZIP root directly contains `manifest.json`.
- Whether the plugin ID and version in the catalog match the manifest, and whether both the catalog and the manifest support the current game and Plugin API.
- Whether the entry point named by the manifest is a DLL that is available inside the package.

These checks mainly guard against broken packages and packages installed in the wrong place; they are not a security audit and do not judge whether a plugin's behavior is appropriate.

## Download or refresh failure

Check these in order:

1. In **Third-party plugins**, confirm that the master switch, the matching plugin source and the HTTPS URL are all correct.
2. Go back to **Available** and click refresh. When the network fails, the page may keep showing the cached list from the last successful fetch.
3. Read the compatibility or install failure reason shown on the page.
4. Check the download, extraction and manifest errors in `Anomaly\anomaly-platform.log`.

If the problem is still unresolved, see [Troubleshooting and FAQ](troubleshooting.md#third-party-plugin-download-and-updates).

## For plugin authors

To get your plugin onto the **Available** page, publish a ZIP with the correct root structure and maintain a `pluginmaster.json`. For the full workflow, see [Publishing third-party plugins](../developer-guide/plugin-distribution.md).
