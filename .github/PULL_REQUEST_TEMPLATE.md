## Change

Describe the behavior, ownership boundary, and evidence.

## Validation

List the exact configure, build, test, sanitizer, packaging, or external commands run.

## Public API review

Complete this section when the change touches public SDK headers, service behavior, Manifest/IPC/
Repository schemas, capabilities, or documented plugin contracts. Otherwise write `Not applicable`.

- RFC and target release:
- API Steward / Service Owner / Release Owner approvals:
- ABI snapshot and C/C++ contract result:
- Layout, enum, service ID/version, calling convention, allocation:
- Thread, ordering, reentrancy, blocking, stop deadline:
- Owner, generation, handle/table/buffer lifetime:
- Status codes, partial writes, retry and fail-closed behavior:
- Capability, schema negotiation, Profile/Feature availability:
- Old SDK/new Host and new SDK/old Host compatibility:
- API reference, ownership docs, examples, limitations, migration:

Unexplained binary or semantic API differences block merge and release. See
`docs/sdk/api-governance.md`.
