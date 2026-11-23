# Loopback web applications

`anomaly.web` v1 requires the `web` capability. The public C header is
`anomaly/sdk/services/web.h`. Runtime owns the loopback HTTP listener and
network I/O; this service has no game or Profile dependency.

The service graph's `anomaly.internal.web-server` node represents optional
listener initialization. Its Ready state means initialization has completed,
not that HTTP endpoints are available. Public `anomaly.web` can only be
queried after the versioned provider is published. Listener/publication failure
is logged with its reason and does not stop unrelated Runtime services.

Call `register_application` from Lifecycle/Worker with a package-relative asset
directory such as `assets/web`, a callback and its user pointer. Each plugin
generation may register one application. Runtime Worker preloads up to 64 files
and 8 MiB in total, requires `index.html`, and rejects traversal and reparse
points. Static requests can return 503 while this preload is pending.

`application_url` uses the two-call NUL-terminated string protocol and returns
`http://127.0.0.1:<port>/plugins/<plugin-id>/#token=<session-token>`. The OS assigns
the port. The token belongs to this registration; do not persist or log it. The
page reads the fragment and supplies `X-Anomaly-Token` on every `/api/` request.
Runtime also verifies Host and any supplied Origin. Static assets are public to
local clients; API responses do not enable CORS.

Runtime dispatches each authenticated GET/POST API request to Worker while holding
the plugin's generation lease. Paths include a leading slash, such as
`/api/state`. Request bodies are limited to 64 KiB. Callbacks must enqueue game
work for Game update and serve GET requests from already published snapshots.

Each request invokes the callback exactly once. Runtime supplies a 1 MiB writable
response buffer. Set `body_size`, `http_status` (200..599), and JSON/TEXT content
type without replacing the buffer pointer. Request/response storage is borrowed
only for the callback duration. HTTP success does not establish game success.

`unregister_application` is Lifecycle/Worker only. It removes admission and drains
callbacks already in flight. Calling it from that application's own callback
returns `CONFLICT`; post a lifecycle intent instead. Scope ledger revocation
also removes forgotten registrations before generation unload. A 504 timeout
does not prove that an admitted callback had no effect; do not automatically
repeat state-changing commands.
