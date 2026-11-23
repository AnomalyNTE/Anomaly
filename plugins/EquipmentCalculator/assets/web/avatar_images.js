const records = new Map();
const current = new Map();
const pendingStatuses = new Set(["queued", "loading", "exporting"]);

function roleId(character) {
  const observed = character?.observed || character;
  return String(observed?.item_id || character?.id || observed?.role_id || "");
}

function sourcePath(character) {
  const observed = character?.observed || character;
  return observed?.head_icon_big_asset_path || observed?.head_icon_asset_path || "";
}

function canonicalPath(value) {
  if (typeof value !== "string") return "";
  let path = value.trim();
  const quoted = path.match(/['"]([^'"]+)['"]/);
  if (quoted) path = quoted[1];
  path = path.split(":", 1)[0];
  const suffix = path.indexOf(".", path.lastIndexOf("/") + 1);
  return (suffix < 0 ? path : path.slice(0, suffix)).toLowerCase();
}

function key(id, source) { return `${id}\n${canonicalPath(source)}`; }

export function avatarUrlFor(character) {
  const id = roleId(character);
  if (current.has(id)) return current.get(id).url || "";
  return character?.avatar_url || character?.observed?.avatar_url || records.get(key(id, sourcePath(character)))?.url || "";
}

export function createAvatarImages({ api, changed }) {
  let desired = [], active = 0, session = null, refreshFinished = false;

  async function decodeAvatar(url) {
    if (typeof url !== "string" || !url) throw new Error("The avatar image is missing.");
    const image = new Image(); image.decoding = "async"; image.src = url;
    await image.decode();
    return image;
  }

  function setSource(record, source, fingerprint) {
    if (canonicalPath(record.source) === canonicalPath(source)) {
      if (fingerprint) record.fingerprint = fingerprint;
      return;
    }
    record.source = source || ""; record.fingerprint = fingerprint || "";
    const cached = records.get(key(record.id, record.source));
    record.status = cached ? "completed" : record.source ? "idle" : "unavailable";
    // Keep the last decoded skin visible until a changed source is ready.
    if (cached) { record.url = cached.url; record.image = cached.image; }
  }

  function publish(record, source, url, image) {
    records.set(key(record.id, source), { url, image });
    if (current.get(record.id) !== record || canonicalPath(record.source) !== canonicalPath(source)) return false;
    record.url = url; record.image = image; record.status = "completed";
    return true;
  }

  async function loadCachedPage(avatars) {
    const pending = [];
    for (const avatar of avatars) {
      const record = current.get(String(avatar.role_id));
      if (!record) continue;
      setSource(record, avatar.source_asset_path, avatar.source_fingerprint);
      if (avatar.status === "completed" && record.status !== "completed") {
        const source = record.source;
        pending.push(decodeAvatar(avatar.avatar_url).then((image) => publish(record, source, image.src, image)));
      } else if (avatar.status === "unavailable") record.status = "unavailable";
    }
    const decoded = await Promise.allSettled(pending);
    if (decoded.some((result) => result.status === "fulfilled" && result.value)) changed();
  }

  async function loadCached() {
    let cursor = 0, snapshot;
    do {
      const page = await api(`api/avatars/cache/${cursor}`);
      if (!page || typeof page.snapshot_id !== "string" || !page.snapshot_id || !Array.isArray(page.avatars)) throw new Error("The avatar cache page is invalid.");
      if (snapshot !== undefined && page.snapshot_id !== snapshot) throw new Error("The avatar cache source changed.");
      snapshot = page.snapshot_id;
      // Publish each cache page before waiting for the rest or touching Game.
      await loadCachedPage(page.avatars);
      if (page.next_cursor === null) break;
      if (!Number.isSafeInteger(page.next_cursor) || page.next_cursor <= cursor) throw new Error("The avatar cache cursor did not advance.");
      cursor = page.next_cursor;
    } while (true);
  }

  async function refreshMetadata() {
    const job = await api("api/avatars/refresh", {});
    const deadline = Date.now() + 15000;
    do {
      const result = await api(`api/avatars/refresh/${encodeURIComponent(job.id)}`);
      if (result.stale) return false;
      if (result.status === "completed") {
        if (!Array.isArray(result.avatars)) return false;
        for (const avatar of result.avatars) {
          const record = current.get(String(avatar.role_id));
          if (record) setSource(record, avatar.source_asset_path, avatar.source_fingerprint);
        }
        return true;
      }
      if (result.status !== "queued" && result.status !== "running") return false;
      await new Promise((resolve) => setTimeout(resolve, 400));
    } while (Date.now() < deadline);
    return false;
  }

  async function openSession() {
    try { await loadCached(); } catch { /* The verified metadata job can still fill genuine cache misses. */ }
    let verified = false;
    try { verified = await refreshMetadata(); } catch { /* Keep the local skin when the live account is unavailable. */ }
    if (!verified) for (const record of desired) if (record.status !== "completed") record.status = "unavailable";
    refreshFinished = true; changed(); pump();
  }

  function pump() {
    if (!refreshFinished) return;
    while (active < 2) {
      const record = desired.find((entry) => !entry.running && (entry.status === "idle" || pendingStatuses.has(entry.status)));
      if (!record) return;
      record.running = true; ++active;
      load(record).finally(() => { record.running = false; --active; pump(); });
    }
  }

  async function load(record) {
    const source = record.source;
    try {
      while (current.get(record.id) === record && record.source === source) {
        const result = await api(`api/avatars/${encodeURIComponent(record.id)}`);
        if (current.get(record.id) !== record || record.source !== source) return;
        if (String(result.role_id) !== record.id || canonicalPath(result.source_asset_path) !== canonicalPath(source)) {
          record.status = "unavailable"; return;
        }
        if (result.status === "completed") {
          const image = await decodeAvatar(result.avatar_url);
          if (publish(record, source, image.src, image)) changed();
          return;
        }
        record.status = result.status;
        if (!pendingStatuses.has(result.status)) return;
        await new Promise((resolve) => setTimeout(resolve, 700));
      }
    } catch {
      if (record.source === source) record.status = "failed";
    }
  }

  return {
    sync(characters, selectedId) {
      const next = new Map(); desired = [];
      for (const character of characters) {
        const id = roleId(character), source = sourcePath(character);
        if (!id || next.has(id)) continue;
        let record = current.get(id);
        if (!record) record = { id, source: "", observedSource: source, fingerprint: "", status: "idle", url: "", running: false };
        if (canonicalPath(record.observedSource) !== canonicalPath(source) || !record.source) setSource(record, source);
        record.observedSource = source;
        next.set(id, record); desired.push(record);
      }
      const updated = next.size !== current.size || [...next].some(([id, record]) => current.get(id) !== record);
      current.clear(); for (const [id, record] of next) current.set(id, record);
      if (selectedId) desired.sort((left, right) => Number(right.id === String(selectedId)) - Number(left.id === String(selectedId)));
      if (updated) changed();
      // A page lifetime owns one metadata check. State polls, role selection and
      // focus changes only reuse it; they never ask Game to scan again.
      if (!session && desired.length) session = openSession();
      else pump();
    },
  };
}
