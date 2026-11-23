export function createRoleBoard({ node, t, allowedCells = [], lockedCells = [], editable, changed }) {
  const allowed = new Set(allowedCells.map((cell) => cell.join(":"))), locked = new Set(lockedCells.map((cell) => cell.join(":")));
  let mode = "cells";
  const element = node("section", "role-board-editor"), heading = node("div", "role-board-heading"); heading.append(node("h3", "", t("web.roles_board")));
  const select = node("select"); select.setAttribute("aria-label", t("web.roles_board_mode"));
  for (const [value, key] of [["cells", "web.roles_board_cells"], ["locks", "web.roles_board_locks"]]) { const option = node("option", "", t(key)); option.value = value; select.append(option); }
  select.hidden = !editable; select.onchange = () => { mode = select.value; render(); }; heading.append(select);
  const grid = node("div", "role-board-grid"), status = node("p", "meta");
  element.append(heading, node("p", "meta", t(editable ? "web.roles_board_hint" : "web.roles_board_native")), grid, status);
  const buttons = [];
  for (let row = 1; row <= 5; row++) for (let column = 1; column <= 5; column++) {
    const key = `${row}:${column}`, button = node("button", "role-board-cell"); button.type = "button";
    button.onclick = () => {
      if (!editable) return;
      if (mode === "locks") { if (locked.has(key)) locked.delete(key); else locked.add(key); }
      else if (!locked.has(key)) { if (allowed.has(key)) allowed.delete(key); else allowed.add(key); }
      render(); changed?.(value());
    };
    buttons.push({ key, row, column, button }); grid.append(button);
  }
  function value() { const coordinates = (set) => [...set].sort().map((cell) => cell.split(":").map(Number)); return { allowed_cells: coordinates(allowed), locked_cells: coordinates(locked) }; }
  function render() {
    for (const { key, row, column, button } of buttons) {
      button.disabled = !editable || (mode === "cells" && locked.has(key));
      button.classList.toggle("enabled", allowed.has(key)); button.classList.toggle("protected", locked.has(key));
      button.setAttribute("aria-pressed", String(mode === "locks" ? locked.has(key) : allowed.has(key)));
      button.setAttribute("aria-label", t(locked.has(key) ? "web.roles_board_cell_locked" : "web.roles_board_cell", row, column, t(allowed.has(key) ? "web.roles_board_enabled" : "web.roles_board_disabled")));
      button.textContent = `${row},${column}`;
    }
    status.textContent = t("web.roles_board_count", allowed.size); status.classList.toggle("invalid", editable && allowed.size !== 20);
  }
  render(); return { element, value, valid: () => !editable || allowed.size === 20 };
}
