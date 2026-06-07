# Shell (bundled plugin)

Nodes from the in-tree **`plugins/shell`** plugin (logic module `pipnode_shell.so`). Unlike the rest of pipnode's nodes, these **deliberately spawn external programs**: each one is a periodic `PnAutoTrigger` source that shells out via `g_spawn_sync()` on a background worker thread and reshapes the captured stdout/stderr into a `PnMessage`. This is the documented exception to the project's "pure C, no helper subprocesses" rule — the whole *point* of these nodes is to run real CLI tools (`df`, `free`, `lxc-ls`, `tmux`, or an arbitrary one-liner), so there is no protocol to port into C. All five register in `pn-shell-plugin.c:64-68`.

Common shape across all five:
- **Base class** — every node subclasses core **`PnAutoTrigger`**, inheriting the background worker thread plus `period` (uint seconds, 1–3600) and `autostart` (bool, CONSTRUCT_ONLY, default TRUE). Default `period` is **5 s** for all five (`PN_*_DEFAULT_PERIOD`).
- **Ports** — all are `has_input=FALSE`, `has_output=TRUE` (pure sources; none consume).
- **Look** — all share the steel-blue body `{0.42,0.62,0.86}`; icons differ per node (FontAwesome 4.x only).
- **`host` routing** — every node carries a `host` string property defaulting to `""` (`PN_SHELL_HOST_DEFAULT`, `pn-shell-host.h:39`). Empty / `localhost` / `127.0.0.1` / `::1` run locally; any other value routes the command through **passwordless ssh** (`pn_shell_host_is_local`, `pn-shell-host.c:22`; `pn_shell_wrap_argv` prepends `ssh -o BatchMode=yes -o ConnectTimeout=5 <host>`, `pn-shell-host.c:36`). `BatchMode=yes` is load-bearing — it disables every interactive prompt, so a remote hop only works with a pre-installed key. The empty value is never coerced to `localhost`; the dialog paints the local machine's name as a grey hint instead (`pn_param_spec_set_hostname_hint`).
- **Thread safety** — `host` (and Shell Command's `shell_command`, Tmux Monitor's session/line-limit/cache) are read on the worker thread and written from the main thread, so each node guards them with its own `GMutex`.

**Two-tier split (TODO #23, Phase 8).** The plugin ships a GUI companion **`pipnode_shell-gui.so`** (`pn-tmux-monitor-gui.c`) exporting `pn_plugin_gui_init` (`pn-tmux-monitor-gui.c:368`). Only **Tmux Monitor** needs it — the editor loads the logic `.so` (registering every `PnAutoTrigger` GType), then dlopens the sibling companion which installs Tmux Monitor's `build_property_editor` + `build_class_tab` dialog vfuncs onto the already-registered class. The other four have no custom dialog (the generic introspection editor handles their `host`/`period`), so they need no companion. `pipnode-run` never loads the companion, so all five run headless. Because the logic `.so` is loaded `G_MODULE_BIND_LOCAL`, the companion cannot see the node's private struct — so the live session list crosses the barrier as a **read-only `sessions` (`G_TYPE_STRV`) property**: the logic half exposes it through `get_property` (`pn-tmux-monitor.c:1020-1027`), and the companion's combo reads it via `g_object_get(target, "sessions", …)` and repopulates on `notify::sessions` (`pn-tmux-monitor-gui.c:76, 242`). That property is the entire logic↔GUI seam.

---

## Shell Command
**Purpose** — Periodic arbitrary-command runner: every `period` seconds it spawns one user-supplied one-liner and emits the combined output. `PnAutoTrigger` subclass (`pn-shell-command.c:43`); trigger at `:157`.
**When to use** — Poll any shell command (a script, a curl, a custom probe) on a timer and feed its text/exit-status into a flow. The general-purpose node of the family; the others are pre-baked specialisations.
**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-shell-command.c:333-334`).
**Settings** —
- `shell-command` (string, default `NULL`/empty) — the one-liner to run (`:336`). While empty the node is red "configuration required" and **emits nothing** (`:58-72, 180-185`).
- `host` (string, default `""`) — local, or remote via ssh as above (`:344`).
- inherited `period` (default **5 s**, `:28`) and `autostart`.
**Emits / consumes** — Timer-driven source (no input). Each tick spawns **`/bin/sh -c <command>` locally** (so pipes/redirections/`$VAR` work without tokenising), or remote: the command string is handed to ssh as one argv element so the *remote* login shell interprets it (`:198-211`, spawn `:215`). `${nodeclass}`/`${nodename}`/`${hostname}` are interpolated first via `pn_node_expand_vars` (`:189`); other `${…}` is left for the shell. Writes only **`data.success`** (exit status == 0) and **`data.output`** (stdout+stderr concatenated) (`:238-240`). **Writes no `data.value`.**
**Gotchas** — **This node executes arbitrary shell code** by design — `/bin/sh -c` on whatever string is configured, on a 5 s timer, with the user's full privileges (and on the remote host's shell when routed via ssh). Treat the `shell-command` field as code, not data. Spawn failures (e.g. ssh down) land the error text in `output` with `success=FALSE`. fa-terminal icon (U+F120).

## Df Command
**Purpose** — Periodic `df` runner that also pre-parses the output into a structured table. `PnAutoTrigger` subclass (`pn-df-command.c:59`); trigger `:361`, table builder `:216`.
**When to use** — Watch filesystem usage on a timer and drive a table/threshold/gauge off named cells without writing a parser.
**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-df-command.c:488-489`).
**Settings** — `host` only (string, default `""`, `:491`), plus inherited `period` (default **5 s**, `:31`) / `autostart`. No command string — hard-wired.
**Emits / consumes** — Timer-driven. Each tick spawns `df` (base argv `{ "df", NULL }`, `:374`; `G_SPAWN_SEARCH_PATH`, ssh-wrapped when remote). Writes **`data.success`**, **`data.output`** (combined), and **`data.table`** — a `PnTableModel`-shaped JSON object (`header` + `rows`, each cell `{text,name}`) built only on success (`NULL` on failure, `:402-407`). The table is fixed at **6 logical columns** (`:46`) — Filesystem / 1K-blocks / Used / Available / Use% / Mounted-on — folding tail tokens into the last column so a mount path with spaces stays one cell (`:169`). Each cell carries a sanitised `name` like `dev_nvme0n1p2.use` (`:110`). No `data.value`.
**Gotchas** — Subprocess, not `/proc` parsing. On non-zero exit, `output` carries the error text but `table` is the empty-table shape. fa-hdd-o icon (U+F0A0).

## Free Command
**Purpose** — Periodic `free` runner emitting a parsed memory table. `PnAutoTrigger` subclass (`pn-free-command.c:45`); trigger `:314`, builder `:180`.
**When to use** — Track RAM/swap on a timer and address `mem.available` / `swap.used` by name downstream.
**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-free-command.c:441-442`).
**Settings** — `host` only (`:444`), plus inherited `period` (default **5 s**, `:32`) / `autostart`.
**Emits / consumes** — Timer-driven. Spawns `free` (base argv `{ "free", NULL }`, `:327`, ssh-wrapped when remote). Writes **`data.success`**, **`data.output`** (combined), **`data.table`** (success-only, `:355-360`). Column count is **derived from the header line** (no fixed width like Df): column 0 is the synthetic `name` row-label column (`Mem:`/`Swap:`), columns 1..N are the sanitised header words (`total`, `used`, `free`, `shared`, `buff/cache`→`buff_cache`, `available`). Short rows (Swap) are padded so consumers never hit ragged rows (`:279-287`). No `data.value`.
**Gotchas** — Real subprocess, not `/proc/meminfo` parsing. Cell names look like `mem.available` / `swap.used`. fa-microchip icon (U+F2DB).

## Lxc Ls Command
**Purpose** — Periodic `sudo lxc-ls -f` runner emitting a parsed container table. `PnAutoTrigger` subclass (`pn-lxc-ls-command.c:45`); trigger `:336`, builder `:201`.
**When to use** — Monitor LXC containers (state, IPs, autostart) on a timer and drive a table/status panel.
**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-lxc-ls-command.c:469-470`).
**Settings** — `host` only (`:472`), plus inherited `period` (default **5 s**, `:32`) / `autostart`.
**Emits / consumes** — Timer-driven. Spawns **`sudo lxc-ls -f`** verbatim (base argv `{ "sudo", "lxc-ls", "-f", NULL }`, `:355`, ssh-wrapped when remote). Writes **`data.success`**, **`data.output`** (combined), **`data.table`** (success-gated, `:383-388`). Column count derived from the `lxc-ls -f` header (NAME / STATE / AUTOSTART / GROUPS / IPV4 / IPV6 / …); extra tail tokens fold into the last column (`:154`). No `data.value`.
**Gotchas** — Runs **`sudo`** — the host user (and, for a remote `host`, the remote user) must have **passwordless sudo** for `lxc-ls`; there is no tty, so a password prompt simply fails and the error lands in `output` with `success=FALSE` (`:349-355`). fa-cubes icon (U+F1B3).

## Tmux Monitor
**Purpose** — Periodically captures the visible content of a named tmux session (local or remote-over-ssh) and emits only the **new** lines since the last tick. `PnAutoTrigger` subclass (`pn-tmux-monitor.c:90`); trigger `:803`. The one shell node with a custom GUI dialog (companion `pn-tmux-monitor-gui.c`).
**When to use** — Tail a long-running tmux job's screen into a flow (log view, alerting) without an agent on the box — just passwordless ssh + tmux.
**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-tmux-monitor.c:1107-1108`).
**Settings** —
- `host` (string, default `""`) — the box the tmux server runs on; local or ssh (`:1110`). Changing it clears the cached session list/selection and re-runs the enumerator (`:500`).
- `tmux-session` (string, default `""`) — the session name to capture (`:1119`). **Empty = red error state, trigger emits nothing** (`:109, 579, 830-835`). The GUI renders this as a live combobox populated from the `sessions` seam.
- `line-limit` (uint, range **1–100000**, default **50**) — trailing scrollback lines captured per tick (`tmux capture-pane -S -<N>`) (`:1127`).
- inherited `period` (default **5 s**, `:30`) / `autostart`.
- **Read-only / not serialised**: `busy` (bool — TRUE while the enumerator thread runs; `PnNodeDialog` overlays a spinner on `notify::busy`, `:1141`), `last-error` (string — drives the red status row, `:1150`), and **`sessions`** (`G_TYPE_STRV` — the logic↔GUI read seam for the session combo, `:1164`).
**Emits / consumes** — Timer-driven. Each tick (when a session is set) spawns **`tmux capture-pane -p -T -S -<line-limit> -t <session>`** (`:846-868`); remote invocations are `g_shell_quote`-wrapped into one argv element so tmux's `#`-bearing format strings survive ssh's space-join (`:188`). Trailing blank rows are stripped, then the capture is **diffed against the previous tick** and only the new tail is emitted (`tm_compute_delta`, `:627`). On emit (topic **`tmux`**) it writes **`data.success`**, **`data.output`** (the delta), **`data.host`** (real local hostname when host empty, else the host), **`data.session`** (`:939-947`). Separately, a one-shot **enumerator thread** runs `tmux list-sessions -F '#S'` (`:389-391`) on construction, on host change, and whenever the cache is empty (`:785`), feeding the `sessions` property.
**Gotchas** — **Stays silent on no change**: an unchanged pane → empty delta → no message (`:911-912`), so it doesn't spam downstream while a screen is idle. **Persistent failures are latched**: the first error of an outage is emitted, then suppressed until the text changes or a capture succeeds (`:920-932`) — so a dead session/ssh doesn't repeat every 5 s. Real subprocess (`tmux`), needs passwordless ssh for a remote `host`. The GUI host entry commits **deferred** (only on Enter / focus-out, not per keystroke) to avoid one ssh hop per character (`pn-tmux-monitor-gui.c:139-179`). fa-television icon (U+F26C).
