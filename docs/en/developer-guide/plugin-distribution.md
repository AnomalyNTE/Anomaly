# Publishing third-party plugins

This page continues from [Plugin development](plugin-development.md): the plugin builds and runs, and the next step is getting it onto Anomaly's **Plugins > Available** page.

Online distribution involves two JSON files, one on the publisher side and one on the consumer side:

```text
Publisher side
  pluginmaster.json              plugin index, lists versions and ZIP download URLs
  plugin ZIP                     manifest.json, plugin.dll and assets
  icon                           optional, accessed directly by IconUrl

Consumer side
  Anomaly\plugin-repositories.json  records one or more pluginmaster.json URLs
```

Publishers maintain and host `pluginmaster.json`; users usually add its URL in the interface, and the Runtime saves that URL into `plugin-repositories.json`. The plugin catalog, ZIPs and icons are all served over HTTPS. They do not use the signed `repository.json` index that Profiles use, and they must not be packed as `.anomaly-package`.

## 1. Get the plugin package working first

The following assumes CMake has already output the directory package to `build\package\MyPlugin`:

```powershell
anomaly-plugin validate .\build\package\MyPlugin
anomaly-plugin pack .\build\package\MyPlugin --output .\dist\packed
anomaly-test-host --plugin .\dist\packed\com.example.my-plugin --reload 10 --ticks 60
```

`anomaly-plugin pack` creates a directory named after the manifest ID and generates `package.sha256`. Its output is still a directory, not the ZIP that the third-party plugins page can download.

For the exact build and TestHost usage, see [Plugin development](plugin-development.md#8-local-development-loop).

## 2. Build the download ZIP

Zip the **contents** of the package directory; do not wrap the top-level directory inside it:

```powershell
$Packed = '.\dist\packed\com.example.my-plugin'
$Zip = '.\dist\com.example.my-plugin-1.2.0.zip'
Compress-Archive -Path "$Packed\*" -DestinationPath $Zip -Force
```

When you open the ZIP, the top level should show:

```text
manifest.json
plugin.dll
package.sha256
assets\               optional
locales\              optional
other native DLLs     optional
```

This layout cannot be installed, because `manifest.json` is not at the ZIP root:

```text
com.example.my-plugin\
  manifest.json
  plugin.dll
```

Check the manifest once more before publishing:

- `id` is a stable dotted lowercase ID; do not change it in later versions.
- `version` is valid SemVer and matches the version you are about to publish.
- `games` contains `nte`, and `api.major` matches the target Runtime.
- `entry` points to a DLL that actually exists inside the ZIP.
- `license`, dependencies, services and capabilities are all filled in.

## 3. Write pluginmaster.json

The root of `pluginmaster.json` must be a JSON array. Usually a plugin ID carries only its current version:

```json
[
  {
    "Name": "My Plugin",
    "InternalName": "com.example.my-plugin",
    "Version": "1.2.0",
    "Author": "Example Author",
    "Punchline": "Shows one useful piece of information in game.",
    "Description": "Explain what problem the plugin solves, how to use it, and any known limitations.",
    "Games": ["nte"],
    "ApiMajor": 1,
    "Tags": ["ui", "utility"],
    "RepoUrl": "https://github.com/example/my-plugin",
    "IconUrl": "https://example.com/my-plugin/icon.png",
    "DownloadLinkInstall": "https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip",
    "DownloadLinkUpdate": "https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip",
    "AcceptsFeedback": true
  }
]
```

JSON field names are case-sensitive, and comments or trailing commas are not supported. With several plugins in the index, keep adding objects to the same array; do not wrap the root in a `plugins` key or any other field.

See the [Anomaly Plugin Template's `pluginmaster.json`](https://raw.githubusercontent.com/AnomalyNTE/Anomaly-Plugin-Template/main/pluginmaster.json) for an online example the Runtime can read as-is.

### Required fields

| Field | Requirement |
| --- | --- |
| `Name` | Plugin name shown to users; must not be empty. |
| `InternalName` | Must exactly match `id` in `manifest.json`. |
| `Version` | Must exactly match the manifest version and be valid SemVer. |
| `DownloadLinkInstall` | HTTPS address that downloads the ZIP directly. |

### Recommended fields

| Field | Purpose |
| --- | --- |
| `Author` | Author or maintainer. |
| `Punchline` | One-line summary in the catalog; state the purpose as directly as possible. |
| `Description` | Fuller feature set, usage and limitations. |
| `Games` | Currently should be `["nte"]`; used to judge compatibility before download. |
| `ApiMajor` | Currently `1`; used to judge compatibility before download. |
| `Tags` | Short tag metadata describing the plugin category. |
| `RepoUrl` | Source code, issue tracker or project homepage. |
| `IconUrl` | HTTPS address of the plugin icon. |
| `AcceptsFeedback` | Whether the plugin accepts user feedback. |

The parser also accepts `DownloadLinkUpdate`. Both install and update currently use `DownloadLinkInstall` as the download address, so you can omit it or keep it identical to `DownloadLinkInstall`.

An entry with missing fields is skipped; when the whole file is not a valid JSON array, that plugin source fails to refresh. The field contract is defined by [`include/anomaly/plugin_list.hpp`](../../../include/anomaly/plugin_list.hpp).

### Correspondence with manifest.json

The following fields describe the same release package. Identity, version and API major are checked at install time; the game compatibility range should also stay consistent:

| `pluginmaster.json` | `manifest.json` / release artifact | Requirement |
| --- | --- | --- |
| `InternalName` | `id` | Must be identical. |
| `Version` | `version` | Must be identical and use SemVer. |
| `Games` | `games` | Both should include the target game; currently `nte`. |
| `ApiMajor` | `api.major` | Should be identical and match the Plugin API major version of the target Runtime. |
| `DownloadLinkInstall` | released ZIP | Must be an HTTPS URL that downloads the ZIP for that version directly. |

`Name`, `Author` and the description fields are for catalog display; after installation the manifest inside the package is authoritative. When you change the version, update at least the manifest `version`, the index `Version` and the ZIP URL together.

## 4. Host the online plugin source

The index can live in a GitHub repository, on GitHub Pages, or on any HTTPS static site that returns file content directly. With GitHub, this split is recommended:

```text
example/anomaly-plugins
  pluginmaster.json       commit to the default branch, keep the URL stable
  assets/my-plugin.png    optional icon

example/my-plugin
  Releases/v1.2.0
    com.example.my-plugin-1.2.0.zip
```

The matching URLs should be direct content or direct download addresses:

| Purpose | Example URL |
| --- | --- |
| Plugin source | `https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json` |
| Icon | `https://raw.githubusercontent.com/example/anomaly-plugins/main/assets/my-plugin.png` |
| Plugin ZIP | `https://github.com/example/my-plugin/releases/download/v1.2.0/com.example.my-plugin-1.2.0.zip` |
| Project homepage | `https://github.com/example/my-plugin` |

Do not use `https://github.com/example/anomaly-plugins/blob/main/pluginmaster.json` as the plugin source; that is a web page address returning HTML, not raw JSON. The ZIP should use the Release asset download address as well, and must not point at the Release detail page.

After uploading, fetch the index and the ZIP again from the public URL, and confirm that the index parses, that the target entry exists, and that the download really is an archive:

```powershell
$IndexUrl = 'https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json'
$PluginId = 'com.example.my-plugin'

$Entries = @(Invoke-RestMethod -Uri $IndexUrl)
$Entry = $Entries | Where-Object { $_.InternalName -eq $PluginId }
if ($null -eq $Entry) { throw "plugin is missing from the online index: $PluginId" }

Invoke-WebRequest -Uri $Entry.DownloadLinkInstall -OutFile .\dist\online-package.zip
Expand-Archive .\dist\online-package.zip .\dist\online-package -Force
Test-Path .\dist\online-package\manifest.json
```

The last line must print `True`. Then run `anomaly-plugin validate` against the extracted directory, which surfaces manifest and package structure problems at the same time.

## 5. Configure the Runtime to use the online plugin source

On the user side, the recommended path is **Plugins > Third-party plugins**, click "Add source", enter the HTTPS URL of `pluginmaster.json` and apply. The Runtime writes the equivalent `Anomaly\plugin-repositories.json`:

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": false,
  "repositories": [
    {
      "url": "https://raw.githubusercontent.com/example/anomaly-plugins/main/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Fixed at `1`. |
| `enabled` | Master switch for the third-party plugin feature. When `false`, no source is refreshed. |
| `allowInsecureSources` | Keep it `false` for public distribution; set it to `true` only for local `file://` testing. |
| `repositories` | Plugin source list; each entry carries an index URL and its own enable switch. |
| `repositories[].url` | Direct HTTPS address of `pluginmaster.json`. |
| `repositories[].enabled` | Whether to refresh that plugin source. |

For multiple online plugin sources, add more objects to `repositories`. After applying, go back to **Plugins > Available** and refresh; there is no need to copy the publisher's `pluginmaster.json` into the Runtime directory, and do not write plugin sources into `repository.json`.

## 6. Order of a new release

A normal release follows this order:

1. Update the manifest version and build the Release package.
2. Run `validate` and `anomaly-test-host`.
3. Generate the ZIP and confirm the root directory structure.
4. Upload the ZIP to a versioned address that will not be overwritten, for example a GitHub Release asset.
5. Download it once more in a browser and confirm that the link really returns the ZIP, not a login page or an HTML error page.
6. Update the version and download address in `pluginmaster.json` last.

Publishing the ZIP first and editing the catalog last keeps users from seeing a new version while the file is not ready yet.

Do not reuse and overwrite an old version URL. Immutable version links are easier to troubleshoot, and they let users know exactly which build they have.

## 7. Test a plugin source locally

`file://` is for controlled local testing only. Quit the game first, then temporarily edit `Anomaly\plugin-repositories.json` in the Runtime directory:

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": true,
  "repositories": [
    {
      "url": "file:///C:/work/my-plugin/repository/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

`DownloadLinkInstall` in the local `pluginmaster.json` can also use `file:///C:/.../plugin.zip`. After restarting the Runtime, refresh in **Plugins > Available** and walk through install, enable, update and uninstall.

Set `allowInsecureSources` back to `false` when testing is done. Configuration and addresses for public distribution must all use HTTPS.

## 8. What the installer verifies

After the user clicks install, the Runtime:

1. Downloads the ZIP and limits the size of the archive and of the extracted content.
2. Rejects dangerous ZIP entries such as absolute paths, `..` traversal and case-duplicated targets.
3. Parses the manifest at the root.
4. Verifies the plugin ID, version, game and Plugin API.
5. Confirms that the entry DLL is inside the package.
6. Replaces the old version through a staging and backup directory; on failure the original installation is kept.

These are format and consistency checks, not a code security audit. Flat `pluginmaster.json` plugin sources currently carry no publisher signature and do not verify `package.sha256`. Publishers should protect the plugin source repository, use immutable Release assets, and publish source code and release notes so users can judge the origin.

## Common publishing problems

| Symptom | Check first |
| --- | --- |
| The plugin does not appear under "Available" | Whether `pluginmaster.json` is an array; whether all four required fields are present; whether the ID and version formats are correct. |
| The whole plugin source fails to refresh | Whether the plugin source URL returns JSON directly rather than a GitHub `blob` page, a login page or 404 HTML; whether the JSON contains comments, trailing commas or a wrong field hierarchy. |
| The plugin shows as incompatible | Whether `Games` and `ApiMajor` match the manifest and the target Runtime. |
| Installation fails after download | Whether `DownloadLinkInstall` downloads the ZIP directly; whether the ZIP has `manifest.json` at its root; whether the ID, version and entry DLL match. |
| A new version does not appear under "Updates" | Whether the new `Version` is really higher than the installed version; whether the catalog and the manifest were both updated. |
| Local `file://` is rejected | Whether `allowInsecureSources` is enabled only in the test configuration, and whether the index and the ZIP both use complete file URIs. |

## Related documentation

- [Plugin development](plugin-development.md)
- [Manifest and capabilities](../api-reference/manifest-and-capabilities.md)
- [Third-party plugins user guide](../user-guide/third-party-plugins.md)
- [Configuration reference](../user-guide/configuration.md)
