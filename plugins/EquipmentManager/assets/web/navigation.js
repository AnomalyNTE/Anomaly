export function createNavigation({ mount, entries, t, node, features, readPage, savePage, ready }) {
  let active = "";
  const buttons = new Map();
  const groups = new Map();
  const tools = node("details", "navigation-tools"), summary = node("summary", "primary-navigation-item", t("web.tools")), menu = node("div", "navigation-menu");
  menu.id = "tool-navigation-menu"; summary.setAttribute("aria-controls", menu.id);
  tools.append(summary, menu);
  for (const entry of entries) {
    const button = node("button", "primary-navigation-item", t(entry.label));
    button.type = "button"; button.setAttribute("aria-controls", entry.element.id);
    button.onclick = () => { savePage(entry.id); show(entry.id); tools.open = false; };
    buttons.set(entry.id, button);
    if (entry.group === "tools") {
      const groupId = entry.toolGroup || "calculate";
      if (!groups.has(groupId)) {
        const section = node("section", "navigation-menu-section"), label = node("h2", "navigation-menu-label", t(`web.tools_${groupId}`));
        const items = node("div", "navigation-menu-items"); section.append(label, items); menu.append(section);
        groups.set(groupId, { section, items, entries: [] });
      }
      const group = groups.get(groupId); group.entries.push(entry); group.items.append(button);
      if (!tools.isConnected) mount.append(tools);
    }
    else mount.append(button);
  }
  document.addEventListener("click", (event) => { if (tools.open && !tools.contains(event.target)) tools.open = false; });
  tools.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && tools.open) { event.preventDefault(); tools.open = false; summary.focus(); }
  });
  function available(entry) { return !entry.feature || (features()[entry.feature] === true && ready()); }
  function show(requested) {
    const target = entries.find((entry) => entry.id === requested && available(entry)) || entries[0];
    summary.textContent = t("web.tools");
    summary.title = target.group === "tools" ? t(target.label) : "";
    summary.classList.toggle("active", target.group === "tools");
    for (const entry of entries) {
      const selected = entry === target;
      entry.element.hidden = !selected;
      buttons.get(entry.id).classList.toggle("active", selected);
      buttons.get(entry.id).setAttribute("aria-current", selected ? "page" : "false");
    }
    if (target.id !== active) { active = target.id; target.controller?.activate(); }
  }
  return {
    go(id) { savePage(id); show(id); tools.open = false; },
    sync() {
      for (const entry of entries) {
        const button = buttons.get(entry.id); button.hidden = !available(entry); button.textContent = t(entry.label);
        entry.controller?.sync();
      }
      for (const group of groups.values()) group.section.hidden = !group.entries.some(available);
      tools.hidden = !entries.some((entry) => entry.group === "tools" && available(entry));
      show(readPage() || entries[0].id);
    },
  };
}
