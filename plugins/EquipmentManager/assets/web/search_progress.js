const phaseKeys = {
  queued: "web.search_progress_queued",
  reading_model: "web.search_progress_reading_model",
  preparing: "web.search_progress_preparing",
  searching: "web.search_progress_searching",
  coverage: "web.search_progress_coverage",
  improving: "web.search_progress_improving",
  alternatives: "web.search_progress_alternatives",
  blueprints: "web.search_progress_blueprints",
  computing: "web.search_progress_computing",
  finished: "web.search_progress_finished",
};
const stopKeys = {
  node_budget: "web.multi_stop_node_budget",
  time_budget: "web.multi_stop_time_budget",
  combination_budget: "web.multi_stop_combination_budget",
  candidate_limit: "web.multi_stop_candidate_limit",
  solution_limit: "web.search_progress_solution_limit",
};
const count = (value) => typeof value === "number" && Number.isFinite(value) && value >= 0;

export function createSearchProgress({ mount, jobController, t, node }) {
  let dismissedJobId = "", signature = "";
  const locale = document.documentElement.lang || "en";
  const integers = new Intl.NumberFormat(locale, { maximumFractionDigits: 0 });
  const seconds = new Intl.NumberFormat(locale, { maximumFractionDigits: 3 });
  const percentages = new Intl.NumberFormat(locale, { style: "percent", maximumFractionDigits: 0 });
  const heading = node("div", "calculation-progress-heading"), phase = node("strong", "calculation-progress-phase"), elapsed = node("span", "calculation-progress-elapsed");
  phase.setAttribute("role", "status"); phase.setAttribute("aria-live", "polite");
  const cancel = node("button", "button quiet"), dismiss = node("button", "button quiet calculation-progress-dismiss", "×");
  cancel.type = "button"; cancel.onclick = () => jobController.cancel();
  dismiss.type = "button"; dismiss.setAttribute("aria-label", t("web.dismiss"));
  dismiss.onclick = () => { dismissedJobId = jobController.state().job.id; render(); };
  heading.append(phase, elapsed, cancel, dismiss);
  const context = node("p", "calculation-progress-label"), notice = node("p", "calculation-progress-label"), budgets = node("div", "calculation-progress-budgets"), metrics = [];
  for (const [usedField, limitField, labelKey, time] of [
    ["budget_elapsed_ms", "time_limit_ms", "web.search_progress_time_budget", true],
    ["nodes", "max_nodes", "web.search_progress_node_budget", false],
    ["combinations", "combination_budget", "web.search_progress_combination_budget", false],
  ]) {
    const element = node("div", "calculation-progress-metric"), label = node("div", "calculation-progress-label"), value = node("span", "calculation-progress-used"), bar = node("progress");
    label.append(node("span", "", t(labelKey)), value); bar.max = 1; bar.setAttribute("aria-label", t(labelKey)); element.append(label, bar); budgets.append(element);
    metrics.push({ element, value, bar, usedField, limitField, time });
  }
  const indeterminate = node("progress", "calculation-progress-indeterminate");
  indeterminate.setAttribute("aria-label", t("web.search_progress_title")); indeterminate.hidden = true;
  mount.append(heading, context, notice, budgets, indeterminate);

  function render() {
    const { job, active, posting, starting, cancelling, applied, error } = jobController.state();
    const progress = starting ? null : job.progress;
    const next = JSON.stringify([job.id, job.status, progress, job.result?.search, job.message, active, posting, starting, cancelling, applied, error, dismissedJobId]);
    if (signature === next) return; signature = next;
    mount.hidden = (applied && !starting) || (!active && !starting && (!progress || dismissedJobId === job.id));
    if (mount.hidden) return;
    const terminal = !active && !starting, search = job.result?.search;
    const limited = terminal && job.status === "completed" && search?.exhaustive === false;
    let phaseKey = phaseKeys[progress?.phase] || (job.status === "queued" ? phaseKeys.queued : phaseKeys.computing);
    if (starting) phaseKey = "web.search_progress_starting";
    else if (cancelling) phaseKey = "web.search_progress_cancelling";
    else if (terminal) phaseKey = job.status === "failed" ? "web.optimize_status_failed" : job.status === "cancelled" ? "web.optimize_status_cancelled" : limited ? "web.search_progress_limited" : "web.search_progress_finished";
    const phaseText = t(phaseKey);
    if (phase.textContent !== phaseText) phase.textContent = phaseText;
    mount.dataset.status = limited ? "budget" : cancelling ? "cancelling" : job.status;
    elapsed.hidden = !count(progress?.elapsed_ms);
    elapsed.textContent = elapsed.hidden ? "" : t("web.search_progress_elapsed", seconds.format(progress.elapsed_ms / 1000));
    cancel.hidden = terminal; cancel.disabled = posting || cancelling || !active;
    cancel.textContent = t(cancelling ? "web.search_progress_cancelling" : "web.optimize_cancel");
    dismiss.hidden = !terminal;

    const parts = [];
    if (count(progress?.roles_completed) && count(progress?.roles_total) && progress.roles_total > 0) parts.push(t("web.search_progress_roles", integers.format(progress.roles_completed), integers.format(progress.roles_total)));
    if (count(progress?.current_group) && count(progress?.groups_total) && progress.current_group > 0 && progress.groups_total > 0) parts.push(t("web.search_progress_group", integers.format(progress.current_group), integers.format(progress.groups_total)));
    else if (count(progress?.groups_visited) && count(progress?.groups_total) && progress.groups_total > 0) parts.push(t("web.search_progress_groups_visited", integers.format(progress.groups_visited), integers.format(progress.groups_total)));
    context.hidden = !parts.length; context.textContent = parts.join(" · ");
    const stopKey = terminal && stopKeys[search?.stop_reason];
    notice.textContent = error || (job.status === "failed" ? job.message : "") || (stopKey ? t(stopKey) : "");
    notice.hidden = !notice.textContent; notice.classList.toggle("error", Boolean(error) || job.status === "failed");

    let visibleMetrics = 0;
    for (const metric of metrics) {
      const used = progress?.[metric.usedField], limit = progress?.[metric.limitField];
      metric.element.hidden = !count(used) || !count(limit) || limit <= 0;
      if (metric.element.hidden) continue;
      ++visibleMetrics;
      const ratio = used / limit, format = metric.time ? (value) => seconds.format(value / 1000) : (value) => integers.format(value);
      metric.value.textContent = t("web.search_progress_usage", format(used), format(limit), percentages.format(ratio));
      metric.bar.value = Math.min(1, ratio); metric.bar.setAttribute("aria-valuetext", metric.value.textContent);
    }
    budgets.hidden = !visibleMetrics;
    indeterminate.hidden = (!active && !starting) || visibleMetrics > 0;
    indeterminate.setAttribute("aria-valuetext", phaseText);
  }

  jobController.subscribe(render); render();
  return { render };
}
