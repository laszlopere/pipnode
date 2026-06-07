# Image Processing (bundled plugin)

Nodes from the in-tree **`plugins/image`** plugin (loadable module `pipnode_image.so`; **no GUI companion** — every node paints with the default node renderer, and gdk-pixbuf is an allowed core dep, so the whole plugin is single-tier core). They are a family of **55 pure image-transform filters** (`pn-image-plugin.c:124-182`). Each one receives a **`PnImageMessage`**, processes the carried `GdkPixbuf` in plain C (no external process, no subprocess), and emits a **new** `PnImageMessage` carrying the result — so an effect drops in between a **File Drop** source and a **File Viewer** sink and you see the transform applied live.

All 55 are registered in `pn-image-plugin.c:104` (`pn_plugin_init`). They share one driver + pixel toolkit in `pn-image-ops.c` (declared in `pn-image-ops.h`), so each node file is just its kernel/maths plus GObject boilerplate. The palette sorts them into eight subgroups under the top-level **Image Processing** group via a slash-separated `category` path (`pn-image-ops.h:29-37`): **Color · Adjust · Blur · Sharpen · Edge Detection · Composite · Geometry · Stylize**.

**Uniform look (verified in source):** every node sets, in *both* `class_init` and `init`, the same icon `"\xef\x83\x90"` (FontAwesome `fa-magic` U+F0D0, the magic wand) and the same body colour `(PnColor){ 0.50, 0.45, 0.70, 1.0 }` — a muted violet (e.g. `pn-image-grayscale.c:61-62`, `pn-image-gaussian-blur.c:133-134`). None override it. So the whole family is visually identical on the canvas; you tell them apart by the class-name label, not by colour or glyph.

---

## The shared base (read this once)

There is **no GObject base class** — every node is a direct `G_DEFINE_TYPE (…, PN_TYPE_NODE)` subclass of core `PnNode`. What they share instead is the **driver functions** in `pn-image-ops.c`, which every node's `receive` vfunc delegates to. There are two driver shapes.

### How the image rides on the message

The pixbuf does **not** travel in a `data.*` bag member. It rides on the message **object type**: a `PnImageMessage` (a `PnMessage` subclass) carries the `GdkPixbuf` directly, fetched with `pn_image_message_get_pixbuf()` and rebuilt with `pn_image_message_new()` (`pn-image-ops.c:604`, `:696`). The `data.*` bag carries only **metadata** describing it (see "Emits" below). The **File Viewer** sink and **File Drop** source speak the same `PnImageMessage` contract, which is why they interoperate ref-shared with no re-read.

### Single-input driver — `pn_image_node_process()`  (`pn-image-ops.c:762`)

The shared `PnNodeClass.receive` body for a one-image filter. Given the node, the borrowed message, a short `label` (the effect name used in the output summary), and a `PnImageTransformFn` (`GdkPixbuf *(*)(GdkPixbuf *src, PnNode *node)`):

- **Non-image pass-through** — if the message is not a `PnImageMessage` or carries a NULL pixbuf, it is emitted **unchanged on the main thread** and the stage is transparent to non-image traffic (`pn-image-ops.c:773-778`).
- **Off-thread pixel work** — otherwise the transform runs on a **`GTask` worker thread** (a large image's O(w·h·kernel) work would freeze the GTK main loop). `GTask` holds a ref on the node so it can't finalise mid-flight (`pn-image-ops.c:633`, `:690`).
- **Coalescing** — at most **one** transform runs per node at a time. An image arriving mid-transform is stashed in a single "pending" slot (latest wins); a burst (e.g. dragging a settings slider) collapses to one queued run instead of a pile of threads (`pn-image-ops.c:783-791`).
- **Processing glow** — `pn_node_processing_begin/end` brackets the work so the node glows while busy, held across a coalesced burst and closed when the queue drains (`pn-image-ops.c:799`, `:740`).

### Two-input driver — `pn_image_node_process2()`  (`pn-image-ops.c:949`)

The **Composite** family (Blend, Multiply, Screen, …) combines **two** images. These nodes call `pn_node_set_n_inputs(node, 2)` in `init` (e.g. `pn-image-blend.c:153`) — so they have **two input ports** — and route via `pn_node_current_input()` to tell the driver which port a message landed on (`pn-image-multiply.c:68`).

The driver **latches the most recent pixbuf per port** and only schedules work once **both** ports hold an image; then it runs the `PnImageBlendFn` `(a, b, node)` on a worker thread (`pn-image-ops.c:986-1004`). Bursts coalesce through a `dirty` flag. Non-image messages pass through without disturbing the latched inputs (`pn-image-ops.c:962-967`).

**How the second image is supplied:** it is **the image latched on the node's second input port** — *not* a stored/loaded file and *not* a config field. You wire two upstream image sources into the two ports. Almost every composite then delegates to the shared **`pn_image_compose()`** (`pn-image-ops.c:507`): the **bigger image (by pixel area; a tie keeps input 0) becomes the canvas**, the smaller is composited into its **top-left corner, never scaled**; over the overlap each channel becomes `base·(1−opacity) + fn(base,over)·opacity`, the canvas keeps its own pixels where the overlay doesn't reach, and the canvas alpha is preserved.

### Pixel toolkit (shared helpers in `pn-image-ops.c`)

Conventions for every helper (`pn-image-ops.h:50-57`): output is a freshly allocated 8-bit RGB(A) pixbuf with the **same geometry and alpha-ness** as the source; any **alpha channel is copied through untouched** (only colour channels are processed); out-of-bounds sampling **clamps to the nearest edge pixel**; results are rounded and clamped to `[0,255]`. Building blocks: `pn_image_map_point` / `pn_image_map_point_xy` (per-pixel point ops), `pn_image_convolve` (generic N×N), `pn_image_gradient` (Sobel/Prewitt edge magnitude on luma), `pn_image_separable` (two-pass Gaussian/Box), `pn_image_median` (rank filter), `pn_image_compose` (blend modes), and `pn_image_rgb_to_hsv`/`hsv_to_rgb`. "Desaturate-first" effects all agree on **Rec.601 luma** `0.299R + 0.587G + 0.114B` (`pn-image-ops.c:49`).

### Ports, settings, error handling (all nodes)

- **Ports** — single-input nodes: `has_input=TRUE`, `has_output=TRUE`. Composite nodes: `n_inputs=2`, `has_output=TRUE`. **Every image node has an output** — they are mid-pipeline filters, never terminal sinks.
- **Settings** — each tunable is a plain `GObject` property, so it appears automatically in the settings dialog. The parameter-free nodes (Grayscale, Invert, Sepia, the edge detectors, the flips/quarter-rotates, Dither, Histogram Equalize, …) expose no settings of their own. **No node uses a custom dialog.**
- **Error / failure handling** — if a transform returns `NULL`, the driver **does not drop the message: it forwards the original through unchanged** (`pn-image-ops.c:726`, `:910`). There is **no `success=FALSE` failure path and no error glyph** — these nodes never paint red. A transform that "can't do anything" (e.g. Gaussian radius < 0.5, Median radius < 1) returns a plain copy, also success.
- **Emits** — on success `emit_image_result` (`pn-image-ops.c:598`) builds a fresh `PnImageMessage` carrying the new pixbuf and sets in the bag: `image=TRUE`, `width`, `height` (the new geometry), `success=TRUE`, and `output` = `"<Label> (W×H)"` (e.g. `"Gaussian Blur (640×480)"`). It also **carries over** `filename`, `path`, and `mimetype` from the source message (when present and string) so downstream savers/labels keep the originating file's identity (`pn-image-ops.c:609-611`).

---

## Worked example — Gaussian Blur

**Purpose** — Soften an image with a true Gaussian kernel, the smooth, halo-free blur. `pn-image-gaussian-blur.c:50` (`gaussian_transform`), dispatched through the shared driver at `:83`.

**When to use** — General-purpose softening / noise reduction before edge work, or a depth-of-field look. Use **Gaussian Blur** for a smooth round falloff; **Box Blur** for a cheaper boxy blur, **Motion Blur** for a directional streak, **Median** for salt-and-pepper removal that keeps edges.

**Ports** — input **and** output (single-input filter): `has_input=TRUE`, `has_output=TRUE` (`pn-image-gaussian-blur.c:137-138, 159-160`). Sits mid-pipeline: `File Drop → Gaussian Blur → File Viewer`.

**Settings**
- `radius` (double, **0.0–50.0, default 2.0**) — blur radius in pixels; the 1-D kernel spans `[-ceil(radius), +ceil(radius)]` and **sigma = radius / 2**. **A radius below 0.5 is a no-op pass-through copy.** `pn-image-gaussian-blur.c:139-143`, `:59`.

**Emits / consumes** — Consumes the input `PnImageMessage`'s pixbuf; ignores the `data.*` bag entirely. Runs a **separable** two-pass blur (`pn_image_separable`, horizontal then vertical) on a worker thread (`pn-image-gaussian-blur.c:75`). On completion emits a fresh `PnImageMessage`: `image=TRUE`, `width`/`height` (unchanged), `success=TRUE`, `output="Gaussian Blur (W×H)"`, plus carried-over `filename`/`path`/`mimetype` (`pn-image-ops.c:613-619`). Alpha preserved.

**Gotchas** — The work is off-thread and **coalesced**: dragging the radius slider over a big image queues only the latest value, not every intermediate one (`pn-image-ops.c:783`). A non-image message passes straight through untouched. There is no failure state — an allocation failure forwards the original image, not an error.

---

## Color  (`Image Processing/Color`) — 4 nodes

| Display name | GType | Operation | Parameter (default) | file |
|---|---|---|---|---|
| Grayscale | `PN_TYPE_IMAGE_GRAYSCALE` | Replace RGB with Rec.601 luma → neutral grey | — | `pn-image-grayscale.c:42` |
| Black & White | `PN_TYPE_IMAGE_BLACK_WHITE` | Hard 1-bit threshold on luma | `threshold` int 0–255 (**128**) | `pn-image-black-white.c:121` |
| Sepia | `PN_TYPE_IMAGE_SEPIA` | Classic warm-brown sepia colour matrix per pixel | — | `pn-image-sepia.c:69` |
| Invert | `PN_TYPE_IMAGE_INVERT` | Photographic negative, `c → 255−c` | — | `pn-image-invert.c:60` |

## Adjust  (`Image Processing/Adjust`) — 10 nodes

| Display name | GType | Operation | Parameter(s) (default) | file |
|---|---|---|---|---|
| Brightness | `PN_TYPE_IMAGE_BRIGHTNESS` | Scale each channel by a multiplier (`c·factor`) | `factor` double 0–4 (**1.0**) | `pn-image-brightness.c:128` |
| Contrast | `PN_TYPE_IMAGE_CONTRAST` | Push channels away from mid-grey 128 | `factor` double 0–4 (**1.0**) | `pn-image-contrast.c:129` |
| Saturation | `PN_TYPE_IMAGE_SATURATION` | Scale colour vs grey (0=greyscale, >1 vivid) | `amount` double 0–4 (**1.0**) | `pn-image-saturation.c:130` |
| Posterize | `PN_TYPE_IMAGE_POSTERIZE` | Quantise each channel to N values (banding) | `levels` int 2–64 (**4**) | `pn-image-posterize.c:131` |
| Solarize | `PN_TYPE_IMAGE_SOLARIZE` | Sabattier: invert channels at/above threshold | `threshold` int 0–255 (**128**) | `pn-image-solarize.c:127` |
| Gamma | `PN_TYPE_IMAGE_GAMMA` | Gamma curve on midtones (<1 darken, >1 lighten) | `gamma` double 0.1–5.0 (**1.0**) | `pn-image-gamma.c:155` |
| Levels | `PN_TYPE_IMAGE_LEVELS` | Remap input black/white points + midtone gamma | `black` int 0–254 (**0**), `white` int 1–255 (**255**), `gamma` double 0.1–5.0 (**1.0**) | `pn-image-levels.c:182` |
| Hue Rotate | `PN_TYPE_IMAGE_HUE_ROTATE` | Spin every pixel's hue (S,V kept) | `degrees` double −180–180 (**0.0**) | `pn-image-hue-rotate.c:120` |
| Vibrance | `PN_TYPE_IMAGE_VIBRANCE` | Saturation boost weighted toward muted pixels | `amount` double −1.0–1.0 (**0.0**) | `pn-image-vibrance.c:123` |
| Histogram Equalize | `PN_TYPE_IMAGE_HIST_EQUALIZE` | Spread luma over full range via CDF (auto-contrast) | — | `pn-image-hist-equalize.c:148` |

## Blur  (`Image Processing/Blur`) — 5 nodes

| Display name | GType | Operation | Parameter(s) (default) | file |
|---|---|---|---|---|
| Gaussian Blur | `PN_TYPE_IMAGE_GAUSSIAN_BLUR` | Separable Gaussian, sigma = radius/2 (<0.5 = no-op) | `radius` double 0–50 (**2.0**) | `pn-image-gaussian-blur.c:139` |
| Box Blur | `PN_TYPE_IMAGE_BOX_BLUR` | Separable box average, window = 2·radius+1 | `radius` int 1–25 (**1**) | `pn-image-box-blur.c:126` |
| Motion Blur | `PN_TYPE_IMAGE_MOTION_BLUR` | Diagonal streak average (1=no-op) | `length` int 1–25 (**9**) | `pn-image-motion-blur.c:127` |
| Median | `PN_TYPE_IMAGE_MEDIAN` | Per-channel median of window (salt-and-pepper) | `radius` int 1–5 (**1**) | `pn-image-median.c:111` |
| Bilateral | `PN_TYPE_IMAGE_BILATERAL` | Edge-preserving smoothing (spatial + colour weight) | `radius` int 1–7 (**3**), `sigma` double 1–150 (**30.0**) | `pn-image-bilateral.c:196` |

## Sharpen  (`Image Processing/Sharpen`) — 4 nodes

| Display name | GType | Operation | Parameter(s) (default) | file |
|---|---|---|---|---|
| Sharpen | `PN_TYPE_IMAGE_SHARPEN` | Standard 3×3 cross unsharp kernel per channel | — | `pn-image-sharpen.c:57` |
| Emboss | `PN_TYPE_IMAGE_EMBOSS` | Directional 3×3 kernel on luma +128 bias (grey relief) | — | `pn-image-emboss.c:58` |
| Edge Enhance | `PN_TYPE_IMAGE_EDGE_ENHANCE` | 8-neighbour sharpen (centre 9, neighbours −1), colour kept | — | `pn-image-edge-enhance.c:59` |
| Unsharp Mask | `PN_TYPE_IMAGE_UNSHARP_MASK` | Subtract a Gaussian mask back to accentuate edges | `amount` double 0–5 (**1.0**), `radius` int 1–15 (**3**) | `pn-image-unsharp-mask.c:186` |

## Edge Detection  (`Image Processing/Edge Detection`) — 4 nodes
All compute on luma and write a grey edge map to R=G=B.

| Display name | GType | Operation | file |
|---|---|---|---|
| Sobel Edge | `PN_TYPE_IMAGE_SOBEL` | Gradient magnitude from the 2 centre-weighted Sobel kernels | `pn-image-sobel.c:64` |
| Prewitt Edge | `PN_TYPE_IMAGE_PREWITT` | Gradient magnitude from the 2 uniform Prewitt kernels (finer) | `pn-image-prewitt.c:64` |
| Laplacian Edge | `PN_TYPE_IMAGE_LAPLACIAN` | 4-neighbour second-derivative kernel (isotropic edges) | `pn-image-laplacian.c:58` |
| Find Edges | `PN_TYPE_IMAGE_FIND_EDGES` | 8-neighbour Laplacian outline (centre 8, neighbours −1; thicker) | `pn-image-find-edges.c:59` |

## Composite  (`Image Processing/Composite`) — 13 nodes  (two-input)
**All take two input ports** (`n_inputs=2`). The second image is the pixbuf **latched on input port 1** (not a file/config). Bigger image = canvas, smaller pasted top-left unscaled; the per-channel `combine` is mixed back over the base by `opacity`. Every one exposes a single `opacity` double **0.0–1.0**; **Blend defaults to 0.5, all twelve others default to 1.0**.

| Display name | GType | Per-channel combine | `opacity` default | file |
|---|---|---|---|---|
| Blend | `PN_TYPE_IMAGE_BLEND` | `over` (straight cross-fade `base·(1−op)+over·op`) | **0.5** | `pn-image-blend.c:132` |
| Multiply | `PN_TYPE_IMAGE_MULTIPLY` | `base·over/255` — darkens | 1.0 | `pn-image-multiply.c:125` |
| Screen | `PN_TYPE_IMAGE_SCREEN` | `255−(255−base)(255−over)/255` — lightens | 1.0 | `pn-image-screen.c:125` |
| Difference | `PN_TYPE_IMAGE_DIFFERENCE` | `|base−over|` | 1.0 | `pn-image-difference.c:126` |
| Darken | `PN_TYPE_IMAGE_DARKEN` | `min(base,over)` | 1.0 | `pn-image-darken.c:124` |
| Lighten | `PN_TYPE_IMAGE_LIGHTEN` | `max(base,over)` | 1.0 | `pn-image-lighten.c:124` |
| Add | `PN_TYPE_IMAGE_ADD` | `base+over` clamped (linear dodge) | 1.0 | `pn-image-add.c:125` |
| Overlay | `PN_TYPE_IMAGE_OVERLAY` | multiply dark / screen light, keyed on **base** | 1.0 | `pn-image-overlay.c:128` |
| Soft Light | `PN_TYPE_IMAGE_SOFT_LIGHT` | Pegtop `(1−2o)b²+2ob` — gentle contrast lift | 1.0 | `pn-image-soft-light.c:129` |
| Hard Light | `PN_TYPE_IMAGE_HARD_LIGHT` | overlay keyed on **over** — strong contrast | 1.0 | `pn-image-hard-light.c:129` |
| Color Dodge | `PN_TYPE_IMAGE_COLOR_DODGE` | `base·255/(255−over)` clamped — brightens | 1.0 | `pn-image-color-dodge.c:130` |
| Color Burn | `PN_TYPE_IMAGE_COLOR_BURN` | `255−(255−base)·255/over` clamped — darkens | 1.0 | `pn-image-color-burn.c:131` |
| Exclusion | `PN_TYPE_IMAGE_EXCLUSION` | `base+over−2·base·over/255` — softer than Difference | 1.0 | `pn-image-exclusion.c:126` |

## Geometry  (`Image Processing/Geometry`) — 8 nodes

| Display name | GType | Operation | Parameter(s) (default) | file |
|---|---|---|---|---|
| Flip Horizontal | `PN_TYPE_IMAGE_FLIP_H` | Mirror left↔right (dims kept) | — | `pn-image-flip-h.c:53` |
| Flip Vertical | `PN_TYPE_IMAGE_FLIP_V` | Mirror top↔bottom (dims kept) | — | `pn-image-flip-v.c:53` |
| Rotate 90° CW | `PN_TYPE_IMAGE_ROTATE_90` | Quarter-turn clockwise (W/H swap) | — | `pn-image-rotate-90.c:52` |
| Rotate 90° CCW | `PN_TYPE_IMAGE_ROTATE_270` | Quarter-turn counter-clockwise (W/H swap) | — | `pn-image-rotate-270.c:52` |
| Rotate 180° | `PN_TYPE_IMAGE_ROTATE_180` | Half-turn (dims kept) | — | `pn-image-rotate-180.c:52` |
| Rotate | `PN_TYPE_IMAGE_ROTATE` | Arbitrary CCW rotate; canvas grows, new corners black | `angle` double −180–180 (**0.0**) | `pn-image-rotate.c:188` |
| Crop | `PN_TYPE_IMAGE_CROP` | Extract a rectangle | `x`/`y` 0–100000 (**0**), `width`/`height` 1–100000 (**256**) | `pn-image-crop.c:151` |
| Resize | `PN_TYPE_IMAGE_RESIZE` | Scale to a target size | `width`/`height` 1–100000 (**256**) | `pn-image-resize.c:122` |

## Stylize  (`Image Processing/Stylize`) — 7 nodes

| Display name | GType | Operation | Parameter(s) (default) | file |
|---|---|---|---|---|
| Vignette | `PN_TYPE_IMAGE_VIGNETTE` | Darken toward corners | `strength` double 0–1 (**0.5**) | `pn-image-vignette.c:138` |
| Pixelate | `PN_TYPE_IMAGE_PIXELATE` | Mosaic: average each square block (censor look) | `block` int 2–64 (**8**) | `pn-image-pixelate.c:164` |
| Add Noise | `PN_TYPE_IMAGE_NOISE` | Add random monochrome grain per pixel | `amount` double 0–1 (**0.2**) | `pn-image-noise.c:129` |
| Dither | `PN_TYPE_IMAGE_DITHER` | Floyd–Steinberg 1-bit B/W error diffusion on luma | — | `pn-image-dither.c:110` |
| Threshold | `PN_TYPE_IMAGE_THRESHOLD` | Hard luma cutoff → black/white | `level` int 0–255 (**128**) | `pn-image-threshold.c:119` |
| Oil Paint | `PN_TYPE_IMAGE_OIL_PAINT` | Most-frequent-intensity bucket per window (painterly) | `radius` int 1–7 (**3**), `levels` int 2–64 (**20**) | `pn-image-oil-paint.c:187` |
| Crosshatch | `PN_TYPE_IMAGE_CROSSHATCH` | Pen-and-ink: four diagonal hatch sets keyed on luma | — | `pn-image-crosshatch.c:71` |

---

**Total: 55 nodes** (4 Color + 10 Adjust + 5 Blur + 4 Sharpen + 4 Edge + 13 Composite + 8 Geometry + 7 Stylize), all registered in `pn-image-plugin.c:124-182`.
