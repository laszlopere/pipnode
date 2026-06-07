# Sound effects (bundled plugin)

The single node from the in-tree **`plugins/sound-effects`** plugin (loadable module `pipnode_sound_effects.so`, GUI companion `pipnode_sound_effects-gui.so`). A **two-tier** plugin: the logic half is GTK-free and plays a chosen audio clip on every message, so a headless host runs it under `pipnode-run`; the editor-only companion adds the clip-download manager + playback-picker dialog. Registered in `plugins/sound-effects/pn-sound-effects-plugin.c:49`; the companion installs its dialog vfunc by resolving the class by name (`g_type_from_name("PnSciFiSound")`) in `pn-sci-fi-sound-gui.c:1265`.

**No audio data ships in the archive** — clips are downloaded on demand into a per-user cache and only referenced by relative path. The two `.so` halves share the GTK-free storage/playback helpers in **`pn-sci-fi-clips.c`**, which is **compiled into both modules** (the editor opens the companion `G_MODULE_BIND_LOCAL`, so a logic-side symbol is invisible across the barrier — each module carries its own copy; single-sourcing the path-traversal-safe resolver is the stated reason, `pn-sci-fi-clips.h:23-35`). There is **no node-state read seam**: the dialog reads the on-disk cache directly, and the download `SoupSession` lives on the companion (`pn-sci-fi-sound-gui.c:346-360`), never on the node.

---

## SciFi Sound

**Purpose** — Sink that plays a sci-fi sound clip whenever **any** message arrives; the message *contents are ignored* — the trigger fact alone fires the clip. `plugins/sound-effects/pn-sci-fi-sound.c:184` (`pn_sci_fi_sound_receive`). Subclasses `PnNode` directly (`pn-sci-fi-sound.c:71`). The themed cousin of the core **Sound** node, with the same overlap/dead-period semantics but a downloadable Star Trek clip library instead of freedesktop theme ids.

**When to use** — An on-brand audible cue on an event (red-alert klaxon on an alarm, transporter chirp on a job done). Use **SciFi Sound** for a downloaded Trek clip; core **Sound** for a freedesktop theme id or arbitrary file; **TTS** to speak `data.output`.

**Ports** — input only; `has_input = TRUE`, `has_output = FALSE` — terminal sink (`pn-sci-fi-sound.c:315-316`, `:356-357`). The inherited `topic` settings row is hidden (no output) (`:318-323`).

**Settings** — Two companion tabs no declarative schema can express (`pn_sci_fi_sound_build_class_tabs`, `pn-sci-fi-sound-gui.c:1224`):
- `clip` (string, default **NULL/unconfigured**) — the played clip as a relative `"<Pack>/<basename>"` id, e.g. `Romulan/romulan_disruptor.mp3` (`pn-sci-fi-sound.c:325-331`). Stored relative so a worksheet saved on one machine still resolves under another `$HOME`. Empty/NULL paints the node red with ❗ (`:87-99, 222`). On the **Playback** tab a Pack combo + Clip combo (populated by scanning the cache) drive it, plus a **Preview** button (`pn-sci-fi-sound-gui.c:1083-1139`).
- `dead-period` (uint 0–3600 s, default **0**) — mandatory silence after each playback; messages within it are dropped (`pn-sci-fi-sound.c:333-340`).

Icon FA `fa-rocket` U+F135, warp-blue body `(0.20,0.55,0.85)` (`pn-sci-fi-sound.c:46`, `:313`).

**Renders / acts** — On a message (ignored), if not already playing and past any dead period and `clip` is set, resolves the clip to an absolute cache path and, if it's a regular file, spawns a media player async (`pn-sci-fi-sound.c:184-207`). Player is the first of **mpv / ffplay / gst-play-1.0 / mpg123 / paplay** on `$PATH`, invoked headless+quiet (`pn_sci_fi_find_player`/`pn_sci_fi_build_argv`, `pn-sci-fi-clips.c:61-109`). The processing glow is lit for the whole playback (`:172, 126`); on finish the dead-period clock is armed (`:120-122`).

**Clips: source / download / cache** — The cache is `$XDG_DATA_HOME/pipnode/sound-effects/startrek/` (`pn_sci_fi_cache_dir`, `pn-sci-fi-clips.c:28-36`), one subdir per pack. The **Sound packs** companion tab shows a per-pack checkbox grid with on-disk counts and Select-all / Clear / **Download selected** / **Delete selected** buttons (`pn-sci-fi-sound-gui.c:1141-1222`). Download fetches the trekcore audio index (`https://www.trekcore.com/audio/`, `:67`), regex-scrapes `.mp3` hrefs, classifies each `(dir, basename)` into one of ~30 packs (Borg, Klingon, Romulan, Red Alert, Transporter, Warp, Doors, … `categories[]` `:96-137`), then GETs each selected-pack clip sequentially into `<cache>/<Pack>/<basename>`, skipping any already on disk (`:500-565, 408-483`). Resumable: a second run re-skips existing files. Delete is a per-pack `rm` of the subdir (`:202-227`).

**Gotchas** —
- **No overlap, dead-period gate:** a message arriving while a clip still plays, or within the dead period after one, is dropped (`pn-sci-fi-sound.c:191-195`) — same as core Sound.
- **Path-traversal safe:** `pn_sci_fi_resolve_path` rejects a leading `/`, a backslash, or `..` so a hand-edited worksheet can't aim the player at an arbitrary file; an unsafe/empty id resolves to NULL and silently plays nothing (`pn-sci-fi-clips.c:38-55`). This is the security reason the resolver is single-sourced into both `.so`.
- **No clips bundled** — an unconfigured node (no download yet) sits red with ❗; the user must download a pack and pick a clip first. Missing player (none of the five on `$PATH`) → `pn_node_log_error`, no sound (`:140-147`).
- The dialog's **Preview** has *no* overlap guard (a deliberate audition always plays) and spawns its **own** player rather than reaching across the `BIND_LOCAL` barrier into the node's playback state (`pn-sci-fi-sound-gui.c:229-297`); it reads the clip via the public `clip` property only.
