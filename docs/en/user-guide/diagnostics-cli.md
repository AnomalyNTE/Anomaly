# Diagnostics CLI

The Runtime opens a local named pipe inside the game process and allows only the current Windows user to access it. `anomaly-cli.exe` connects to this pipe using the game process PID, sends commands to the Runtime verbatim, and writes the returned JSON to standard output.

## Basic usage

```powershell
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
.\anomaly-cli.exe --pid <PID> sections .
.\anomaly-cli.exe --pid <PID> scan . .text "48 8B ?? ?? ?? ?? ?? 48 85 C0"
```

Conventions:

- `.` means the **game main module**.
- The CLI sends text commands directly, and each call returns one line of UTF-8 JSON.
- `snapshot [filename]` writes the result to the `Anomaly` directory; any directory part in the filename is discarded.
- The PID is recorded in `Anomaly\logs\anomaly-runtime.log`.
- `anomaly-cli.exe` currently always connects to `\\.\pipe\LOCAL\Anomaly-<PID>`. If you change `[Analyzer] PipePrefix`, this CLI can no longer connect directly.

## Command reference

### Status and enumeration (read-only)

| Command | Result |
| --- | --- |
| `ping` | Service PID |
| `status` | PID, Runtime root directory, platform mode, and the Runtime diagnostics summary |
| `modules` | Loaded modules, paths, base addresses, image sizes |
| `sections <module\|.>` | PE sections, base addresses, sizes, RWX attributes |
| `regions <module\|.>` | Memory region state, protection, and type |
| `help` | Command list |

### Scanning and cross-references (read-only)

| Command | Result |
| --- | --- |
| `scan <module\|.> <section> <pattern>` | Signature hit addresses in current memory; `pattern` supports `??`, `4?`, and `?F` wildcards |
| `xrefs <module\|.> <target>` | Cross-references pointing to the `target` address |

### UE / Profile status (read-only)

| Command | Result |
| --- | --- |
| `ue` | Active Profile, symbol resolution results, and the Feature Matrix |
| `ue actors <filter\|*> [limit] [cursor]` | When the relevant UE5 Feature is available, read-only paged query of Actor names, classes, and Outers |
| `ue functions <filter\|*> [limit] [cursor]` | When the relevant UE5 Feature is available, read-only paged query of UFunction names, Owners, and parameter metadata |

### Memory reads

| Command | Result |
| --- | --- |
| `read <address> <size>` | Reads up to 1048576 bytes |
| `chain <base> [offset ...]` | Dereferences level by level and applies offsets (pointer chain) |
| `ptr <address>` | Reads one pointer value |
| `f32 <address> [count]` | Reads 32-bit floats |
| `f64 <address> [count]` | Reads 64-bit floats |
| `rip <instruction> <disp_offset> <instruction_size>` | Computes the target address of a RIP-relative instruction |
| `snapshot [filename]` | Writes a JSON snapshot to the `Anomaly` directory |

### Memory modification (dangerous)

| Command | Result |
| --- | --- |
| `write <address> <hex bytes>` | Writes to an existing writable page and returns the original bytes |
| `patch <address> <hex bytes>` | Temporarily switches to RWX, writes, flushes the instruction cache, and restores protection |
| `protect <address> <size> <r\|rw\|x\|rx\|rwx>` | Changes page protection and returns the old protection value |
| `alloc <size> [protection]` | Allocates virtual memory in the target process |
| `free <address>` | Frees a region returned by `alloc` |

> [!TIP]
> The actual output of `help` is authoritative for the command list; different versions may add or adjust commands.

## Calling the JSON protocol directly

When you write your own pipe client, you can send versioned JSON requests. `anomaly-cli.exe` does not use this format; it uses the single-line text command interface described above.

```json
{"protocol":"anomaly.diagnostics","version":1,"type":"request","id":"status-1","command":"status"}
```

A response keeps `protocol`, `version`, `type`, `id`, and `ok`; a successful result is in `result`, and errors use `error` with a stable `code` / `message`.

The Runtime fields in a `status` response contain service, plugin, and resource diagnostics. The exact fields follow the `status` output of the current version.

> [!NOTE]
> This protocol is for **diagnostics**, not plugin-to-plugin IPC. Plugins communicate with each other through [`anomaly.ipc`](../api-reference/ipc.md).

## Related

- [NTE Build Profile](nte-profiles.md) — the `ue` command depends on the active Profile
- [Troubleshooting](troubleshooting.md) — cannot connect to the pipe, commands return errors
