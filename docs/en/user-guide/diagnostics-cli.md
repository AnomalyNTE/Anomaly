# Diagnostics CLI

The Runtime opens a local named pipe inside the game process and allows only the current Windows user to access it. `anomaly-cli.exe` connects to this pipe using the game process PID, sends commands to the Runtime verbatim, and writes the returned JSON to standard output.

## Basic usage

```powershell
.\anomaly-cli.exe --help
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
.\anomaly-cli.exe --pid <PID> sections .
.\anomaly-cli.exe --pid <PID> scan . .text "48 8B ?? ?? ?? ?? ?? 48 85 C0"
```

Conventions:

- `.` means the **game main module**.
- The CLI sends text commands directly, and each call returns one line of UTF-8 JSON; branch on its `ok` field.
- Arguments are joined with single spaces before they are sent, so `scan . .text 48 8B ?? 48 85 C0` works both quoted and split. Arguments must not contain line breaks, and a request may not exceed 64 KiB.
- `snapshot [filename]` writes the result to the `Anomaly` directory; any directory part in the filename is discarded.
- The PID is recorded in `Anomaly\logs\anomaly-runtime.log`.
- `anomaly-cli.exe` currently always connects to `\\.\pipe\LOCAL\Anomaly-<PID>`. If you change `[Analyzer] PipePrefix`, this CLI can no longer connect directly.
- An ordinary console is enough: the Runtime publishes the diagnostics pipe with a medium integrity label, so no administrator rights are needed.

## Help and exit codes

`--help`, `-h`, `/?`, and `help` print the full grammar to standard output and exit with `0`. When the arguments are invalid the help goes to standard error and standard output stays empty.

| Exit code | Meaning |
| --- | --- |
| `0` | The Runtime answered; the JSON `ok` field decides the outcome |
| `2` | Client-side usage error: missing or malformed `--pid`, unencodable argument, request over 64 KiB |
| `3` | Connection failed: `2` means no pipe for that PID, `5` access denied, `231` every instance busy |
| `4` | The pipe read mode could not be configured, or the request could not be written |
| `5` | The response could not be read, or writing to standard output failed |

A connection failure also prints the pipe name and a targeted hint (wrong PID, or a privilege/integrity mismatch).

## Command reference

Argument syntax:

- `<address>`: `0x`-prefixed hexadecimal or decimal, e.g. `0x147e40ba0`. Sizes and offsets share the same parser, so `0x10` and `16` are equivalent.
- `<section>`: PE section name, e.g. `.text`.
- `<pattern>`: hexadecimal bytes; `??` is a whole-byte wildcard, `4?` fixes the high nibble, `?F` the low nibble.
- `<hex bytes>`: space-separated hexadecimal bytes, e.g. `90 90` or `E9 1B 00 00 00`, at most 4096 bytes.
- `<protection>`: `r`, `rw`, `x`, `rx`, `rwx`, or a numeric `PAGE_*` value.

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
| `ue objects <filter\|*> [limit] [cursor]` | When the relevant UE5 Feature is available, read-only paged query of object names, classes, and Outers |
| `ue functions <filter\|*> [limit] [cursor]` | When the relevant UE5 Feature is available, read-only paged query of UFunction names, Owners, and parameter metadata |
| `ue fname <id>` | Resolves an FName id to text |
| `ue ftext <address>` | Reads the FText display string stored at that address |
| `ue combat` | Combat bridge status |
| `ue buffs` | Buff snapshot |

`<filter>` is a substring of a name or class, `*` disables filtering, and `limit` / `cursor` page through the result; the next cursor comes back with the previous page.

### Memory reads

| Command | Result |
| --- | --- |
| `read <address> <size>` | Reads up to 1048576 bytes and returns the raw bytes plus decoded values |
| `chain <base> [offset ...]` | Dereferences level by level and applies offsets (at most 64 offsets), returning the final address |
| `ptr <address>` | Reads one pointer value |
| `f32 <address> [count]` | Reads 32-bit floats; `count` defaults to 3 |
| `f64 <address> [count]` | Reads 64-bit floats; `count` defaults to 3 |
| `rip <instruction> <disp_offset> <instruction_size>` | Computes the target address of a RIP-relative instruction, adding the pointer stored there when readable |
| `snapshot [filename]` | Writes a JSON snapshot to the `Anomaly` directory |

### Memory modification (dangerous)

| Command | Result |
| --- | --- |
| `write <address> <hex bytes>` | Writes to an existing writable page and returns the original bytes |
| `patch <address> <hex bytes>` | Temporarily switches to RWX, writes, flushes the instruction cache, and restores protection |
| `protect <address> <size> <r\|rw\|x\|rx\|rwx>` | Changes page protection and returns the old protection value |
| `alloc <size> [protection]` | Allocates virtual memory in the target process, at most 268435456 bytes, `rw` by default |
| `free <address>` | Frees a region returned by `alloc` |

> [!TIP]
> `anomaly-cli.exe --help` prints the same grammar, argument syntax, and exit codes as this page. The command list in the Runtime's `help` output stays authoritative; different versions may add or adjust commands.

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
