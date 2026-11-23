const sameUid = (left, right) => left != null && right != null && left.slot === right.slot && left.serial === right.serial;

export function sameLoadout(left, right) {
  if (!left || !right || !sameUid(left.character, right.character) || left.placements.length !== right.placements.length) return false;
  if (!(left.core == null && right.core == null) && !sameUid(left.core, right.core)) return false;
  return left.placements.every((placement) => right.placements.some((other) => sameUid(placement.equipment, other.equipment)
    && placement.row === other.row && placement.column === other.column));
}

export function loadoutMatchesInventory(plan, inventory) {
  if (!plan?.character || inventory?.complete !== true || inventory.items_complete !== true) return false;
  const equipped = inventory.items.filter((item) => sameUid(item.character, plan.character));
  if (equipped.length !== plan.placements.length + (plan.core ? 1 : 0)) return false;
  return equipped.every((item) => item.kind === "core" ? sameUid(item.uid, plan.core)
    : plan.placements.some((placement) => sameUid(item.uid, placement.equipment) && item.row === placement.row && item.column === placement.column));
}
