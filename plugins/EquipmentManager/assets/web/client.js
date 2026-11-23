export function createApi({ token, t, account }) {
  return async function api(path, body, options = {}) {
    const headers = { Accept: "application/json", "X-Anomaly-Token": token };
    let encoded;
    if (body !== undefined) {
      const owner = account();
      if (!owner) throw new Error(t("web.workspace_waiting"));
      headers["Content-Type"] = "application/json";
      encoded = JSON.stringify({ ...body, account_id: owner.id, account_generation: owner.generation });
      const size = new TextEncoder().encode(encoded).length;
      if (size > (path === "api/settings" ? 49152 : 65536)) throw new Error(t(path === "api/settings" ? "web.settings_too_large" : "web.request_too_large"));
    }
    let response;
    try {
      response = await fetch(path, { method: body === undefined ? "GET" : "POST", headers,
        ...(body === undefined ? {} : { body: encoded }), cache: "no-store", keepalive: options.keepalive === true });
    } catch { throw new Error(t("web.connection_error")); }
    let data;
    try { data = await response.json(); } catch { throw new Error(t("web.request_failed")); }
    if (!response.ok || data?.error) {
      const failure = new Error(data?.error?.message || data?.message || t("web.request_failed"));
      failure.status = response.status; failure.code = data?.error_code || data?.error?.code || data?.code || "";
      throw failure;
    }
    return data;
  };
}
