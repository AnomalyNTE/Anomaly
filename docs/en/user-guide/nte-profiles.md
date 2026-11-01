# NTE Build Profile

Player position, entity snapshots and teleport all depend on in-game signatures and structure offsets. That data lives in a **Build Profile**, not in `anomaly.ini`.

## What a Profile is

A Profile records signatures, sections, structure offsets, and which symbols each Feature depends on. The Runtime uses it to locate `GWorld`, `GObjects`, `FNamePool` and the Game Tick, then serves UE5 / NTE services to plugins according to validated Features.

Profile directories (relative to `Anomaly\`, see the [`[Profiles]` configuration](configuration.md#profiles)):

| Directory | Priority | Source |
| --- | --- | --- |
| `profiles-local\` | Highest | Local override (placed by you) |
| `state\profiles\managed\` | Middle | Repository-released, signed Profiles |
| `profiles\` | Lowest | Profile bundled with the runtime package |

The Runtime selects **one** active Profile by the priority `local override > repository > bundled`. It selects only by game ID and source priority; it does not compute the fingerprint of the current EXE first and then match a version.

## How the Runtime uses a Profile

> [!IMPORTANT]
> At startup the Runtime does not compute a game PE fingerprint, and it does not use Build identity to select a Profile. When a signature scan is needed, however, it still reads the sections such as `.text` that the Profile specifies.

- The Resolver scans symbols against the active Profile on every startup and runs the validator declared by the Profile on each resolution result.
- After symbols are assembled, a Feature must still satisfy the dependencies, layout validators, ABI and thread conditions declared by the Profile.

### Automatic learning and signature recovery

After a symbol resolves successfully, the Runtime reads up to 128 bytes of x64 instructions and automatically generates an instruction signature for the next recovery, so you do not need to write fallback patterns. Instruction decoding keeps opcodes, register selection, member and stack displacements and ordinary immediates, and wildcards only RIP-relative addresses and relative branch operands. At least 4 complete instructions and 24 fixed bytes are required; a generated signature is written to `state/signatures/<game>/signatures.json` only if it matches the original location uniquely inside the declared executable section.

When the original signature cannot find its target or all candidates fail validation, the Resolver tries the learned records; when there are no records or the learning window matches nothing, it tries to regenerate a signature from the fully decodable instructions in the original pattern, keeping the wildcards the original declaration already had. Recovery candidates must still pass the original symbol validator and the Feature layout and ABI validation. Multiple matches, insufficient evidence or fixed instruction bytes that cannot be matched keep the Feature degraded — it never guesses by similarity; when it meets an unsupported instruction it uses only the evidence decoded completely so far. RIP-relative target addresses are recomputed from the new instructions and old RVAs are not reused. The symbol result of the CLI `ue` command gains `signatureRecovered` and `activePattern` to distinguish a successful recovery from an original signature hit.

Learned records are isolated by module, section, original pattern and active Profile content hash / symbol ID; they do not take part in Profile selection and do not modify the Profile source or the original files. A corrupted cache is ignored, and a failed write is reported as a diagnostic and does not affect resolution results that passed validation. Learning and recovery run during startup resolution, and a stop request aborts the remaining work; a single low-level section scan is still a synchronous call.

Plugin signatures stay where the plugin already keeps them. The existing `anomaly.interop.signature` v1 interface uses the same recoverer, and learned records live in each plugin's state directory under `signature-knowledge/signatures.json`. No plugin, manifest or SDK change is needed, and nothing has to migrate into the overall Profile. Game/Render callbacks do not perform learning, recovery or state-file I/O.

Plugins that also need to learn field offsets can opt into signature v2: declare field seeds and function evidence inside the plugin and the framework resolves the whole group of addresses and the field snapshot, without putting plugin signatures into the overall Profile. HiFi is already wired to PlayingID, Paused, PlayerType, CurrentID and OwnedIDs, keeping the separate degradation boundaries for core playback and the optional album. Older v1 plugins keep their original contract.

This is recovery based on existing instruction evidence and cannot cover arbitrary function rewrites. v1 cannot modify offset constants inside a compiled plugin; v2 updates only fields that are explicitly declared and pass the object-source, access-width, alignment and consistency validation, while other layouts may still need manual adaptation.

## Feature degradation

If there is no active Profile, or some symbol, layout or ABI validation fails:

- Core, UI, Diagnostics and plugins that do not depend on NTE **keep working**.
- Adapter services that depend on failed symbols stay `unavailable` per **Feature**.
- Plugins that declare those services as `optional` still load; only the corresponding function is unavailable.

In **Diagnostics > Developer > NTE Compatibility** in the management UI, or with the CLI `ue` command, you can view the active Profile, symbol resolution results and the Feature Matrix.

## Validate a Profile

`anomaly-profile.exe` can check the Profile format and also generate a PE fingerprint offline. `fingerprint` is used to record validation evidence; the Runtime does not call it at startup.

```powershell
# 1. (Optional) generate the PE / .text diagnostic identity offline
.\anomaly-profile.exe fingerprint C:\path\to\HTGame.exe nte

# 2. Copy the current Profile from the template
Copy-Item .\Anomaly\profiles\nte\nte-build-profile.json.example `
          .\Anomaly\profiles\nte\nte-current.json

# 3. Validate after filling in validated signatures
.\anomaly-profile.exe validate .\Anomaly\profiles\nte\nte-current.json
```

The bundled `nte-current.json` holds the current active declaration.

## Compatibility notes

- **A Profile does not match Build identity automatically.** On-machine validation can record the fingerprint at that moment, but that marker is not a runtime compatibility gate.
- **Mutation services are stricter.** Mutation services (such as player teleport) are published only when their engine ABI / reflection, dependent signatures and the Game-thread gate all pass at the same time, and the Pawn-vtable fallback is forbidden. A single on-machine validation does not constitute cross-version support.
- **When Profile or Feature validation fails**, Core, UI, Diagnostics and plugins that do not depend on NTE remain usable.

> [!NOTE]
> For the current NTE layout (camera, player, teleport point offsets and more), see the bundled [`profiles/nte/nte-current.json`](../../../profiles/nte/nte-current.json); start a new Profile by copying [`profiles/nte/nte-build-profile.json.example`](../../../profiles/nte/nte-build-profile.json.example). For the concrete format of Profiles and signatures, see [`schemas/build-profile.schema.json`](../../../schemas/build-profile.schema.json).

## Related

- [Configuration reference · [Profiles]](configuration.md#profiles)
- [Diagnostics CLI · `ue` command](diagnostics-cli.md#ue--profile-status-read-only)
- [Troubleshooting](troubleshooting.md)
