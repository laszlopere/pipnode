# Ollama (bundled plugin)

The single node from the in-tree **`plugins/ollama`** plugin (loadable module `pipnode_ollama.so`, GUI companion `pipnode_ollama-gui.so`). It is a **two-tier** plugin: the logic half is GTK-free and talks to an [Ollama](https://ollama.com/) server over libsoup-3, so a headless host loads and runs it under `pipnode-run`; the editor-only companion adds the model-picker combo. The node is registered in `plugins/ollama/pn-ollama-plugin.c:53` via the standard `pn_plugin_init`; the companion installs its dialog vfunc by resolving the class by name (`g_type_from_name("PnOllama")`) in `pn-ollama-gui.c:353`.

It used to be a built-in `libpipnode` node; it now lives here and reaches the factory through the plugin path (`pn-ollama-plugin.c:26-31`).

---

## Ollama

**Purpose** — Filter that submits the incoming message's `data.output` string to an Ollama server's `POST /api/generate` endpoint and emits a fresh message whose `data.output` carries the model's completion. The "ask an LLM" node. `plugins/ollama/pn-ollama.c:353` (`pn_ollama_receive`). Subclasses `PnNode` directly (`pn-ollama.c:72`).

**When to use** — Mid-pipeline LLM transform: take some text on `data.output` (a Chat entry, a Shell stdout, a sensor summary), run it through a local or remote model, and pass the reply on — e.g. `Chat → Ollama → Chat`, or `MQTT Source → Format → Ollama → TTS`. The emitted bag matches the "chat-style" contract (`output` + `success` + `from_long_name`) that **Chat** consumes via `sender-path = data/from_long_name`, so it plugs straight into a Chat sink.

**Ports** — `has_input = TRUE`, `has_output = TRUE` (`pn-ollama.c:578-579`, `:647-648`). Filter, not a source/sink.

**Settings** — Five-property dialog; the **Model** row is replaced by the companion's `/api/tags` combo + Refresh button (`pn-ollama-gui.c:328-336`), the rest use default editors:
- `hostname` (string, default **`""`** empty) — Ollama server host/IP. Empty is shown as a grey local-hostname hint (`pn_param_spec_set_hostname_hint`, `pn-ollama.c:590`) and is resolved to the loopback literal **`localhost`** when building the URL (`build_endpoint`, `pn-ollama.c:124-133`) so libsoup always reaches a local server.
- `port` (int 1–65535, default **11434** — Ollama's standard HTTP port) (`pn-ollama.c:43`, `:592-596`).
- `model` (string, default **`""`**, **required**) — model id e.g. `llama3.2:latest`; the companion combo populates it from the server's `/api/tags` listing, sorted, and re-queries on `notify::hostname`/`notify::port` or the Refresh button (`pn-ollama-gui.c:73-149`, `:285-325`). A saved id is kept selectable even if the server doesn't list it yet (`:196-205`).
- `keep-alive` (string, default **`"5m"`**) — forwarded verbatim as the request's `keep_alive`: a duration (`"5m"`, `"1h"`, `"30s"`), `"0"` to unload immediately, or `"-1"` to keep the model resident forever (`pn-ollama.c:44`, `:605-612`). Sent only when non-empty.
- `prefix` / `suffix` (string, both default **`""`**) — concatenated **around** the input as `prefix + data.output + suffix` to carry system/role framing in one node (`pn-ollama.c:382-385`, `:614-626`).

Icon FA `fa-microchip` U+F2DB (FA 4.7 stand-in for the missing `fa-robot`), indigo body `(0.42,0.36,0.72)` (`pn-ollama.c:31`, `:576`).

**Emits / consumes** — *Consumes* string `data.output` only; a missing/non-string `output` is treated as the empty prompt `""` (not dropped) (`pn-ollama.c:373-380`). Sends a non-streaming request (`"stream": false`, `:329-330`) and awaits the full JSON reply off-thread (`soup_session_send_and_read_async` on a session reused across calls, `:414`). On success parses the top-level `.response` string (`:173-222`) and **emits a brand-new message** (own topic/id/created envelope) with: `data.output` = the completion, `data.success` = TRUE, `data.from_long_name` = the node's instance name (else class name) (`pn-ollama.c:295-303`). The processing glow is lit for the whole round-trip (`:412, 243`).

**Gotchas** —
- **Empty hostname silently drops, despite no error glyph.** `refresh_visual_state` marks the node configured as soon as `model` is non-empty — hostname empty is "fine" (`pn-ollama.c:107-118`). But `pn_ollama_receive` returns early if **either** `hostname` *or* `model` is empty (`:369-371`). So with the default empty hostname the node looks healthy yet processes nothing until you type a host (e.g. `localhost`). Only the unconfigured-*model* case paints red + ❗.
- **Failures are logged, not emitted.** Connection failure, non-2xx HTTP, Ollama's `{"error":…}` body (returned with a 2xx), or a parse error all go to `pn_node_log_error` and emit **nothing** downstream (`pn-ollama.c:248-283`, `:206-213`) — i.e. this node does *not* follow the "errors via a `success=FALSE` message" pattern; only the happy path ever emits, always with `success=TRUE`.
- Non-streaming only — long generations block until the whole reply lands (the glow stays on). Concurrent inputs each fire their own request (no in-flight guard); a pending request is cancelled on finalize via the node's `GCancellable` (`:544-547`).
- The `/api/tags` enumeration lives **entirely in the companion** (`ollama_list_models`, `pn-ollama-gui.c:73`); the logic half never queries the model list. The companion drives the node purely through GObject properties (the logic `.so` is `BIND_LOCAL`).
