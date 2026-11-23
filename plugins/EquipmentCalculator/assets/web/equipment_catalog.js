export function observedEquipmentCatalog(inventory) {
  const shapes = new Map(), suits = new Map(), counts = {};
  for (const item of inventory?.items || []) {
    if (item.kind === "module" && item.shape_id && Array.isArray(item.cells) && item.cells.length) {
      shapes.set(item.shape_id, { shape_id: item.shape_id, cells: item.cells });
      counts[item.shape_id] = (counts[item.shape_id] || 0) + 1;
    }
    if (item.kind === "core" && item.suit_id && item.required_shapes_complete === true) suits.set(item.suit_id, {
      id: item.suit_id, name: item.suit_name || item.suit_id, required_shapes: item.required_shapes,
    });
  }
  return { shapes: [...shapes.values()].sort((a, b) => a.shape_id.localeCompare(b.shape_id)), suits: [...suits.values()], counts };
}

export function shapeThumbnail(shape, node) {
  const preview = node("span", "shape-thumbnail"); preview.setAttribute("aria-hidden", "true");
  const rows = shape.cells.map((cell) => cell[0]), columns = shape.cells.map((cell) => cell[1]);
  const minRow = Math.min(...rows), minColumn = Math.min(...columns);
  preview.style.gridTemplateRows = `repeat(${Math.max(...rows) - minRow + 1}, 8px)`;
  preview.style.gridTemplateColumns = `repeat(${Math.max(...columns) - minColumn + 1}, 8px)`;
  for (const [row, column] of shape.cells) {
    const cell = node("i"); cell.style.gridRow = row - minRow + 1; cell.style.gridColumn = column - minColumn + 1; preview.append(cell);
  }
  return preview;
}
