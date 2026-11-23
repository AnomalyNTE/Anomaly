export function createOptimizationJob({ api, t }) {
  let job = { status: "idle" }, posting = false, error = "", readError = "", timer, readPromise = null;
  let appliedJobId = "", cancellingJobId = "", postAction = "";
  const listeners = new Set();
  const active = () => ["queued", "running"].includes(job.status);
  const cancelling = () => active() && Boolean(job.id && (job.id === cancellingJobId || job.cancel_requested === true));
  const emit = () => { for (const listener of listeners) listener(); };
  async function collectResult(first) {
    if (first.status !== "completed" || !Array.isArray(first.result?.blueprints)) return first;
    const total = first.total_results;
    if (!Number.isSafeInteger(total) || total < first.result.blueprints.length || first.page_start !== 0) throw new Error(t("web.request_failed"));
    if (job.id === first.id && job.status === "completed" && job.source_snapshot_id === first.source_snapshot_id &&
        job.total_results === total && job.next_cursor === null && job.result?.blueprints?.length === total) {
      return { ...first, next_cursor: null, result: { ...first.result, blueprints: job.result.blueprints } };
    }
    const blueprints = [...first.result.blueprints]; let page = first;
    while (page.next_cursor !== null) {
      const cursor = page.next_cursor;
      if (!Number.isSafeInteger(cursor) || cursor !== blueprints.length || cursor <= page.page_start) throw new Error(t("web.request_failed"));
      page = await api(`api/optimize/results/${encodeURIComponent(first.id)}/${cursor}`);
      if (page.id !== first.id || page.total_results !== total || page.page_start !== cursor || !Array.isArray(page.result?.blueprints) || !page.result.blueprints.length) throw new Error(t("web.request_failed"));
      blueprints.push(...page.result.blueprints);
    }
    if (blueprints.length !== total) throw new Error(t("web.request_failed"));
    return { ...first, next_cursor: null, result: { ...first.result, blueprints } };
  }
  function refresh() {
    if (readPromise) return readPromise;
    clearTimeout(timer);
    readPromise = (async () => {
      try { job = await collectResult(await api("api/optimize")); readError = ""; }
      catch (failure) { readError = failure.message; }
      finally { readPromise = null; emit(); if (active()) timer = setTimeout(refresh, 700); }
    })();
    return readPromise;
  }
  async function post(path, body, action) {
    if (posting) return;
    posting = true; postAction = action; error = ""; emit();
    try {
      const accepted = await api(path, body);
      // A GET started before this POST may still contain the preceding job.
      // Finish that read before requesting the newly accepted state.
      if (readPromise) await readPromise;
      await refresh(); return accepted;
    }
    catch (failure) { error = failure.message; }
    finally { posting = false; postAction = ""; emit(); }
  }
  return {
    state: () => ({ job, posting, starting: posting && postAction === "run" && !active(), active: active(), cancelling: cancelling(), error: error || readError, applied: Boolean(job.id && job.id === appliedJobId) }),
    markApplied(id) { if (id && id === job.id && job.status === "completed") { appliedJobId = id; emit(); } },
    refresh,
    run(path, body) { if (!active()) return post(path, body, "run"); },
    async cancel() {
      if (!active() || posting || cancelling()) return;
      cancellingJobId = job.id;
      const accepted = await post("api/optimize/cancel", {}, "cancel");
      if (!accepted) { cancellingJobId = ""; emit(); }
      return accepted;
    },
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener); },
  };
}
