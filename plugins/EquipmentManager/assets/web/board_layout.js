export function boardLayout(allowedCells) {
  if (!allowedCells.length) return { rows: 0, columns: 0, cells: [] };
  const firstRow = Math.min(...allowedCells.map(([row]) => row));
  const firstColumn = Math.min(...allowedCells.map(([, column]) => column));
  return {
    rows: Math.max(...allowedCells.map(([row]) => row)) - firstRow + 1,
    columns: Math.max(...allowedCells.map(([, column]) => column)) - firstColumn + 1,
    cells: allowedCells.map(([row, column]) => ({ row, column, gridRow: row - firstRow + 1, gridColumn: column - firstColumn + 1 })),
  };
}

export function arrangeBoard(element, allowedCells) {
  const layout = boardLayout(allowedCells);
  element.hidden = !layout.cells.length;
  if (layout.cells.length) {
    element.style.gridTemplateColumns = `repeat(${layout.columns}, minmax(0, 1fr))`;
    element.style.gridTemplateRows = `repeat(${layout.rows}, minmax(0, 1fr))`;
    element.style.aspectRatio = `${layout.columns} / ${layout.rows}`;
    element.style.setProperty("--board-columns", String(layout.columns));
    element.style.setProperty("--board-rows", String(layout.rows));
  }
  return layout.cells;
}
