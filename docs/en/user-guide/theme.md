# Themes and backgrounds

The look of the Anomaly interface comes from two parts, and both apply to the **in-game main window** (`Anomaly.Core.dll`) and to **Anomaly Launcher** at the same time:

| Part | What it decides | Where it lives |
| --- | --- | --- |
| **Palette** | Window base color, button accent color, text and border colors | `Anomaly\config\platform-settings.json` |
| **Layout** | Background image, stickers, panel opacity | `Anomaly\config\themes\` |

You pick the palette in the interface and build the layout in the web editor.

**The layout file name decides its scope**: `config/themes/` has two levels — `<scheme-name>.json` (for example `naiwa.json`) is a **theme-specific layout** that applies only under that palette and holds this theme's stickers and its bundled wallpaper; `layout.json` is the **shared layout** and applies to every palette.

**The background image is global**: once you enable and pick an image in Settings, it is written into the shared layout and applies to all palettes — **including themes that ship their own wallpaper** (picking an image overrides the theme's own wallpaper). After you uncheck it, the shared layout no longer keeps a background and each theme's own wallpaper comes back automatically. **Background transparency (the veil, `surfaceOpacity`) is global as well**: the slider in Settings is written into the shared layout, so it applies to every palette and also overrides the value a theme file carries.

**Only the Naiwa preset ships**: `config/themes/naiwa.json` and its three stickers are packaged, because Milk Frog is the default palette and that is what the main window looks like out of the box. Nothing else is preinstalled; the layout is generated the **first time you change the background or a sticker**, so rebuilding and redeploying never overwrite what you picked yourself.

## Built-in palettes

Available under **Settings > Interface > Color palette**:

| Palette | Base color | Accent color | Notes |
| --- | --- | --- | --- |
| Anomaly Hub | Dark | Cyan | Default |
| Naiwa / Milk Frog | `#ffeeba` | `#fcce44` | Cream base color plus a warm yellow accent, a good fit for light background images |
| Other built-in palettes | — | — | See the live preview in the interface |
| Custom | Any | Any | Pick each color individually |

A selection takes effect immediately and is written to `platform-settings.json`; the launcher reads the same settings on its next start, so the two interfaces always agree.

## Three ways to fill a background image

`background.mode` in the layout file decides how the image covers the interface:

| Mode | Behavior | Use it for |
| --- | --- | --- |
| `center` | **Center**: scales proportionally until fully visible, with padding around the edges and no cropping | When you want the whole image visible |
| `crop` | **Crop**: scales proportionally until it covers, then crops the overflow from the center | The most common "wallpaper" effect |
| `stretch` | **Stretch**: stretches straight to the interface size without keeping the aspect ratio | Solid colors, gradients, or images that tolerate distortion |
| `disabled` | Draws no background | Off |

Images support PNG / JPEG / BMP / GIF first frame. **The PNG alpha channel is preserved in full**, so you can use background images with transparent regions; images without an alpha channel are treated as opaque.

### Panel opacity

`surfaceOpacity` controls the opacity of the interface base color layer drawn over the background image (`0`–`1.0`, matching the Background visibility slider in Settings, which runs `0%`–`100%`). `1.0` means the base color is fully opaque and the background image is hidden; `0` means no dimming at all and the image shows at full strength; around `0.6` the background image shows through clearly. Text, borders and accent colors always stay opaque, so readability does not suffer. Stickers are drawn **above** this base color layer, so lowering it never fades them.

The interface stacks from bottom to top in a fixed order:

```
Window base color  →  Background image  →  Veil (surfaceOpacity)  →  Stickers  →  Panels / buttons / text
```

So a solid panel does **not** hide stickers (the opacity of the panel base color is governed by `surfaceOpacity` across the board), and stickers **do not** hide any text or buttons — those are always drawn above the stickers.

### Beyond the window

Stickers are not clipped by the main window: put `x`/`y` outside `0` or `1` and the sticker **spills outside the window**, producing a "naked-eye 3D" pop-out effect. `x`/`y` accept `-4`–`5`, so you can push a sticker up to 4 interface widths off screen.

In in-game overlay mode the overlay covers the whole game window and the overflow is fully visible; in standalone window mode the window hugs the main interface, so how much of the overflow you see depends on the margin around the window.

## Stickers

`stickers` is an array where each entry describes one image placed on the interface:

| Field | Type | Meaning |
| --- | --- | --- |
| `path` | string | Image path relative to `Anomaly\`, which cannot point outside the root directory |
| `x` / `y` | 0–1 | Normalized position of the anchor within the interface (`0.5` is dead center) |
| `width` / `height` | 0–1 | Normalized size; give only one and the other is computed from the image aspect ratio, give both as `0` to use the image's native pixels |
| `anchor` | enum | `center` / `top_left` / `top_right` / `bottom_left` / `bottom_right`, deciding which corner of the sticker `x`/`y` aligns to |
| `rotation` | degrees | Clockwise is positive, rotation is around the sticker's center |
| `opacity` | 0–1 | Opacity of the whole sticker |
| `tint` | `#RRGGBB` | Color modulation, where `#ffffff` means original colors |
| `layer` | enum | `behind` (default, recommended) draws above the veil and below text and buttons, so it is neither opaque nor covers controls; `front` draws **above all content**, and the sticker covers the text and buttons beneath it |
| `flipX` / `flipY` | boolean | Horizontal / vertical mirror |

Up to 64 stickers, and the whole file stays under 256 KB. An image with the same path is read only once.

## Building a layout with the web editor

`tools/theme_layout_editor/index.html` is a **fully offline** single-file editor: double-click it to open it in a browser, with nothing to install and no network access.

1. Open the editor and drag your PNG assets onto the canvas. **A background image is not required**: you can edit without importing one, because the canvas draws a reference skeleton laid out at real proportions — title bar / side navigation / content area / status bar — so you can judge whether a sticker will cover a control.
2. Drag to place, pull a corner to scale, use the rotation handle to turn; the property panel on the right lets you type exact `x`/`y`/size/rotation/opacity/tint and switch `anchor`, `layer` and mirroring. A ring of "outside the window" area surrounds the canvas, so dragging a sticker out there shows the overflow effect directly instead of clipping it.
3. Switch the background fill mode at the top (center / crop / stretch) and adjust `surfaceOpacity`; what you see is what you get.
4. Click **Export JSON** to get the layout file (drop it into `Anomaly\config\themes\`; a file named after a palette is a theme-specific layout, and `layout.json` is the shared layout), or click **Export ImGui** to get equivalent ImGui drawing code (`AddImage` / `AddImageQuad`) you can embed directly in your own interface code.

Keep the canvas size equal to `canvas` (default `1180×700`, the default size of the in-game main window); stickers use normalized coordinates, so positions stay correct after the interface is moved or scaled.

Put the exported file into `Anomaly\config\themes\`; once saved, the in-game interface reloads automatically without a restart. The Settings page also shows the current layout status: background mode, sticker count and panel opacity; when a file has a problem it shows the reason it was rejected.

The launcher reads the same layout on its next start.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| Stickers or the background do not appear | First check that the file is under `Anomaly\config\themes\`, then confirm the file name (`layout.json` or `<scheme-name>.json`) and that the image paths in `assets/images/` all match |
| The Settings page says "Layout rejected: …" | JSON syntax error, `schemaVersion` is not `1`, more than 64 stickers, or a path that escapes the root directory |
| The background image does not show | `background.mode` is `disabled`, or `path` points to a file that does not exist |
| A sticker does not show | Wrong `path`; `width`/`height` are both `0` and the image failed to load; or a sticker on a higher layer covers it |
| The overflowing part of a sticker is not visible | In standalone window mode the window hugs the main interface and the margin around it is too small; in-game overlay mode has no such limit |
| The background image is distorted | `mode` uses `stretch`; change it to `crop` |
| Interface text is hard to read | `surfaceOpacity` is too low or the background image is too busy; raise it, or switch the text to a palette with stronger contrast |
| A sticker is invisible | With `layer: "front"` the sticker draws on the top layer together with its transparent regions; check `path` and `width`/`height`, and make sure another `front` sticker does not cover it entirely |
