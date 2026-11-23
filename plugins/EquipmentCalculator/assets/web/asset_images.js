import { gameImageSprites } from "./game_images.js";
import { avatarUrlFor } from "./avatar_images.js";

const svgNamespace = "http://www.w3.org/2000/svg";
const avatarElements = new WeakMap();
const equipmentElements = new WeakMap();
const atlases = new Map();
let definitions;

function assetKey(value) {
  if (typeof value !== "string") return "";
  let path = value.trim();
  const quoted = path.match(/['"]([^'"]+)['"]/);
  if (quoted) path = quoted[1];
  path = path.split(":", 1)[0];
  const objectSuffix = path.indexOf(".", path.lastIndexOf("/") + 1);
  if (objectSuffix >= 0) path = path.slice(0, objectSuffix);
  return path.toLowerCase();
}

function equipmentSprite(item) {
  for (const field of ["icon_asset_path", "plan_icon_asset_path", "icon_big_asset_path", "icon_small_asset_path"]) {
    const sprite = gameImageSprites[assetKey(item?.[field])];
    if (sprite) return sprite;
  }
  return null;
}

function shapeFallback(item, node) {
  const fallback = node("span", "shape asset-fallback");
  if (item?.kind === "core") { fallback.classList.add("core-symbol"); fallback.textContent = "◫"; }
  else if (Array.isArray(item?.cells) && item.cells.length) {
    const minimumRow = Math.min(...item.cells.map(([row]) => row));
    const minimumColumn = Math.min(...item.cells.map(([, column]) => column));
    // Only this thumbnail is normalized. RPC anchors keep their signed offsets.
    for (const [row, column] of item.cells) {
      const cell = node("i"); cell.style.gridRow = String(row - minimumRow + 1); cell.style.gridColumn = String(column - minimumColumn + 1); fallback.append(cell);
    }
  } else { fallback.classList.add("core-symbol"); fallback.textContent = "?"; }
  return fallback;
}

export function shapeGlyph(item, node, variant = "") {
  if (item?.kind !== "module" || !Array.isArray(item.cells) || !item.cells.length) return null;
  const rows = item.cells.map(([row]) => row), columns = item.cells.map(([, column]) => column);
  const minRow = Math.min(...rows), maxRow = Math.max(...rows), minCol = Math.min(...columns), maxCol = Math.max(...columns);
  const glyph = node("span", `shape-glyph${variant ? ` ${variant}` : ""}`);
  glyph.setAttribute("aria-hidden", "true");
  glyph.style.setProperty("--shape-rows", String(maxRow - minRow + 1));
  glyph.style.setProperty("--shape-cols", String(maxCol - minCol + 1));
  for (const [row, column] of item.cells) {
    const cell = node("i");
    cell.style.gridRow = String(row - minRow + 1);
    cell.style.gridColumn = String(column - minCol + 1);
    glyph.append(cell);
  }
  return glyph;
}

function atlasFor(sprite) {
  let atlas = atlases.get(sprite.atlas);
  if (atlas) return atlas;
  if (!definitions) {
    const root = document.createElementNS(svgNamespace, "svg");
    root.classList.add("equipment-definitions"); root.setAttribute("aria-hidden", "true"); root.setAttribute("focusable", "false");
    root.setAttribute("width", "0"); root.setAttribute("height", "0");
    definitions = document.createElementNS(svgNamespace, "defs"); root.append(definitions); document.body.append(root);
  }
  const image = document.createElementNS(svgNamespace, "image");
  atlas = { id: `equipment-atlas-${atlases.size}`, status: "loading", listeners: new Set() };
  atlases.set(sprite.atlas, atlas);
  image.id = atlas.id; image.setAttribute("width", String(sprite.atlasWidth)); image.setAttribute("height", String(sprite.atlasHeight));
  const finish = (status) => {
    atlas.status = status;
    for (const listener of atlas.listeners) listener(status === "ready");
    atlas.listeners.clear();
  };
  image.addEventListener("load", () => finish("ready"), { once: true });
  image.addEventListener("error", () => finish("failed"), { once: true });
  // Only this shared image loads the packaged PNG. Thumbnails reference its
  // existing decoded resource even when the server sends Cache-Control:no-store.
  image.setAttribute("href", sprite.atlas); definitions.append(image); return atlas;
}

export function equipmentIcon(item, node, variant = "") {
  if (variant === "board" && !equipmentSprite(item)) return null;
  const container = node("span", `equipment-icon${variant ? ` ${variant}` : ""}`);
  container.setAttribute("aria-hidden", "true");
  equipmentElements.set(container, { node, artKey: null, shapeKey: "", fallback: null, svg: null, unsubscribe: null });
  updateEquipmentIcon(container, item); return container;
}

export function updateEquipmentIcon(container, item) {
  const state = equipmentElements.get(container);
  if (!state) return false;
  const shapeKey = JSON.stringify([item?.kind, item?.cells]);
  if (shapeKey !== state.shapeKey) {
    const fallback = shapeFallback(item, state.node);
    if (state.fallback) state.fallback.replaceWith(fallback); else container.prepend(fallback);
    state.fallback = fallback; state.shapeKey = shapeKey;
  }
  const sprite = equipmentSprite(item), artKey = sprite ? JSON.stringify(sprite) : "";
  if (state.artKey === artKey) return true;
  state.unsubscribe?.(); state.svg?.remove(); container.classList.remove("asset-ready");
  state.artKey = artKey; state.svg = null; state.unsubscribe = null;
  if (!sprite) return true;
  const atlas = atlasFor(sprite);
  const svg = document.createElementNS(svgNamespace, "svg");
  svg.setAttribute("viewBox", `${sprite.x} ${sprite.y} ${sprite.width} ${sprite.height}`);
  svg.setAttribute("width", String(sprite.width)); svg.setAttribute("height", String(sprite.height));
  svg.setAttribute("preserveAspectRatio", "xMidYMid meet"); svg.setAttribute("focusable", "false"); svg.setAttribute("aria-hidden", "true");
  svg.classList.add("equipment-artwork");
  const use = document.createElementNS(svgNamespace, "use"); use.setAttribute("href", `#${atlas.id}`);
  svg.append(use); container.append(svg); state.svg = svg;
  const loaded = (ready) => { if (state.artKey === artKey) container.classList.toggle("asset-ready", ready); };
  if (atlas.status === "loading") { atlas.listeners.add(loaded); state.unsubscribe = () => atlas.listeners.delete(loaded); }
  else loaded(atlas.status === "ready");
  return true;
}

export function characterAvatar(character, node, variant = "") {
  const container = node("span", `character-avatar${variant ? ` ${variant}` : ""}`);
  container.setAttribute("aria-hidden", "true");
  container.append(node("span", "avatar-fallback", (character?.name || "?").slice(0, 1)));
  // Avatar URLs are captured from the real game HeadIcon/current appearance.
  // Item cards, portraits and the static equipment atlas are not substitutes.
  avatarElements.set(container, { character, node, source: "" });
  updateAvatar(container); return container;
}

export function updateCharacterAvatar(container, character) {
  const state = avatarElements.get(container);
  if (!state) return false;
  state.character = character;
  container.querySelector(".avatar-fallback").textContent = Array.from(character?.name || "?")[0];
  updateAvatar(container); return true;
}

function updateAvatar(container) {
  const state = avatarElements.get(container);
  if (!state) return;
  const source = avatarUrlFor(state.character);
  if (state.source === source) return;
  state.source = source;
  container.querySelector("img")?.remove(); container.classList.remove("asset-ready");
  if (!source) return;
  const image = state.node("img", "avatar-artwork"); image.alt = ""; image.decoding = "async";
  image.addEventListener("load", () => { if (state.source === source) container.classList.add("asset-ready"); }, { once: true });
  image.addEventListener("error", () => { image.remove(); if (state.source === source) container.classList.remove("asset-ready"); }, { once: true });
  image.src = source; container.append(image);
  if (image.complete && image.naturalWidth > 0) container.classList.add("asset-ready");
}

export function refreshCharacterAvatars() {
  for (const container of document.querySelectorAll(".character-avatar")) updateAvatar(container);
}
