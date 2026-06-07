# Host monitoring (bundled plugin)

Nodes from the in-tree **`plugins/host-monitoring`** plugin (loadable module `pipnode_host_monitoring.so`). It is a **core-only, GTK-free** `.so` that builds against `libpipnode-core` and installs into `$pkglibdir/plugins/`, so a headless server can run it under `pipnode-run` (`pn-host-monitoring-plugin.c:19-31`). All 8 nodes are registered from the single `pn_plugin_init()` entry point (`pn-host-monitoring-plugin.c:65-72`); they used to be built-ins of `libpipnode` and were lifted out into this plugin.

Every node is a **periodic source**: `has_input=FALSE`, `has_output=TRUE`, subclassing core **`PnAutoTrigger`** *directly* (there is no shared base class inside the plugin — the nodes are deliberately kept structurally identical so a future refactor could lift the shared sampling loop into a helper, but today each is standalone). A background worker thread ticks every `period` seconds, samples the host, builds a `PnMessage` on the worker thread, and hands it to `pn_auto_trigger_emit_on_main()` so listeners run on the main thread. They share one **look**: the FontAwesome-4 category, violet body `(0.62, 0.50, 0.78)`, and palette category **"Host monitoring"** (set in both `class_init` and `init` in every file). None has a startup-announce — they only emit on a worker tick.

### The shared host / transport pattern (read once, applies to all 8)

Every node carries a `hostname` string property (default **`""`**, the empty string) wired through `pn_param_spec_set_hostname_hint()`, which makes the settings dialog render `g_get_host_name()` as a grey placeholder hint. `hostname_is_local()` maps NULL, `""`, **and** the literal `"localhost"` (case-insensitive) to the local path; **any other value is treated as a passwordless-SSH target** (`ssh -o BatchMode=yes -o ConnectTimeout=<period-1> -o StrictHostKeyChecking=accept-new <host> cat <procfile>`). The configured-vs-real distinction is cosmetic on the wire: when local, `data.host` and the summary still report the **real machine name** (`g_get_host_name()`), never `"localhost"`, so a worksheet is self-describing (e.g. `pn-load.c:220-225`). The `hostname` string is mutex-guarded (worker thread reads, main thread writes); the delta-based nodes (CPU, Disk I/O, Network I/O) additionally reset their `have_prev` baseline under that lock on any host/device/interface change so a delta is never computed across two different machines/rows.

Standard bag members written by every node: `data.host` (string, resolved display name) and `data.success` (boolean). On success each writes `data.value` (the canonical reading) plus `data.output` (a one-line human summary); on failure `success=FALSE` and `output` carries a single-line reason — preferring the remote SSH **stderr** ("Permission denied (publickey).", "Connection refused") over the generic GError ("Child process exited with code N"). These nodes do **not** set the red error glyph; the failure is surfaced purely through `success`/`output`.

Below, **System Load** is the fully-worked example; the rest are documented as deltas against it, with a compact table for the metric/source/field differences.

## System Load
**Purpose** — Periodic source emitting the 1-minute runqueue-length load average. Subclasses `PnAutoTrigger`; reads `/proc/loadavg` and parses the three averages locale-independently with `g_ascii_strtod` (`pn-load.c:119-144`, trigger `pn-load.c:208`).

**When to use** — A cheap "how busy is this box" heartbeat for a graph or threshold. Reports the load-average proxy, not true CPU utilisation — use the **CPU** node when you need the actual busy fraction.

**Ports** — `has_input=FALSE`, `has_output=TRUE` (`pn-load.c:376-377`).

**Settings** —
- `hostname` (string, default `""`) — local `/proc/loadavg` read, or SSH target; grey local-name hint (`pn-load.c:379-387`).
- Inherited `period` (uint seconds, 1–3600) — **default 5** (`PN_LOAD_DEFAULT_PERIOD`, `pn-load.c:26`; kernel only updates loadavg every 5 s).

**Emits / consumes** — Source: no input; emits every worker tick. On success writes `value` (load1, double), `load5`, `load15`, plus `host`/`success`/`output` ("Load on `<host>` is `<l1>`.") (`pn-load.c:248-260`). On failure: `success=FALSE` + reason in `output`.

**Gotchas** — `value` is the runqueue load average, **not** a percentage. A dead SSH host stalls the worker thread up to `ConnectTimeout` (= `period-1`, min 1 s), not the GUI. Parse failure (truncated file) yields `success=FALSE`.

### The other seven (deltas from System Load)

All seven share the System-Load shape above: `PnAutoTrigger` source, no input, violet body, `hostname` property with the local/SSH pattern + grey hint, `host`/`success`/`output` always written, emit every tick. They differ in the metric, default `period`, extra settings, and the extra `data.*` members:

| Node | Metric source | Default `period` | Extra settings | `data.value` | Other `data.*` on success | file:line |
|---|---|---|---|---|---|---|
| System Load | `/proc/loadavg` | 5 | — | load1 (double) | `load5`, `load15` | `pn-load.c:208`,`:376` |
| CPU | `/proc/stat` jiffy **delta** | 2 | `core` (string, default `""`=aggregate `cpu` row; `"0"`,`"1"`… select `cpu0`,`cpu1`) | busy % (0–100) | `idle`, `iowait` (%), `unit`="%", `core` | `pn-cpu.c:323`,`:566` |
| Memory | `/proc/meminfo` snapshot | 2 | `unit` (enum `PnMemoryUnit`: `%`/`B`/`KByte`/`MByte`/`GByte`, default `%`) | used (% or binary bytes) | `used`,`total`,`available`,`free`,`percent`,`unit` | `pn-memory.c:347`,`:588` |
| CPU Temperature | `/sys/class/hwmon` (CPU drivers), fallback `/sys/class/thermal` | 5 | `aggregation` (enum `PnTempAggregation`: `average`/`maximum`, default `average`) | °C (avg or max) | `average`,`min`,`max`,`count`,`unit`="°C",`hot`,`readings`(JSON array),`aggregation` | `pn-temp.c:640`,`:855` |
| Ambient Temperature | `/sys/class/hwmon` (`acpitz`), fallback `/sys/class/thermal` (chipset/DPTF) | 5 | `aggregation` (enum `PnAmbientAggregation`: `average`/`maximum`, default `average`) | °C (avg or max) | same field set as CPU Temperature | `pn-ambient.c:637`,`:842` |
| Disk I/O | `/proc/diskstats` sector **delta** | 2 | `device` (string, default `""`=all whole disks), `unit` (enum `PnDiskIoUnit`: `B`/`KByte`/`MByte`/`GByte` per sec, **default GByte/sec**) | total throughput | `read`,`write`,`unit`,`device` | `pn-disk-io.c:539`,`:815` |
| Network I/O | `/proc/net/dev` byte **delta** | 2 | `interface` (string, default `""`=all non-loopback), `unit` (enum `PnNetIoUnit`, **default MByte/sec**) | total throughput | `rx`,`tx`,`unit`,`interface` | `pn-net-io.c:455`,`:713` |
| Net Connections | counting **shell script** over `/proc/<pid>/net/tcp{,6}` | 5 | — | ESTABLISHED TCP count (int) | — (just `value`) | `pn-connections.c:278`,`:429` |

### CPU
**Purpose** — Real CPU busy fraction (0–100 %), computed as the per-tick delta of `/proc/stat` cumulative jiffy counters, not the loadavg proxy (`pn-cpu.h:26-46`). `busy = user+nice+system+irq+softirq+steal`, `idle = idle+iowait`; guest/guest_nice are *not* double-counted (already folded into user/nice) (`pn-cpu.c:59-71, 245-249`).
**When to use** — When you need actual utilisation, optionally per-core. Distinguishes "100 % of one core" from "saturated 16 cores", which System Load cannot.
**Settings** — `hostname`; `core` (string, default `""` = aggregate `cpu` row → whole machine; `"0"`/`"1"`… → `cpu0`/`cpu1` for a single core, `pn-cpu.c:579-588`); inherited `period` (default 2).
**Gotchas** — **First tick after construction or a host/core change emits `value=0` with output "…warming up (next tick has the first delta)"** — there is no prior baseline (`pn-cpu.c:430-446`); a `0.0` warm-up reading is not a genuine idle CPU. Changing `core` or `hostname` resets `have_prev` (`pn-cpu.c:127,148`). Emits `idle`, `iowait`, `unit`="%", and the trimmed `core` label.

### Memory
**Purpose** — Instantaneous memory pressure from `/proc/meminfo`; `used = MemTotal - MemAvailable` (`pn-memory.c:386-389`). No baseline state — each tick stands alone (snapshot, not a delta).
**Settings** — `hostname`; `unit` (enum `PnMemoryUnit`: `%` default, or `B`/`KByte`/`MByte`/`GByte`, `pn-memory.c:601-614`); inherited `period` (default 2).
**Gotchas** — Byte units are **binary** (1 KByte = 1024 B — RAM-vendor/`free`/`htop` convention), **deliberately divergent** from the decimal-SI Disk/Net I/O nodes (`pn-memory.c:98-115, 601`). `data.percent` is always the percentage regardless of `unit`. In `%` mode the `total`/`available`/`free` fields are reported as percentages (total=100) to keep the field shape stable across modes (`pn-memory.c:399-417`).

### CPU Temperature
**Purpose** — Aggregated CPU die/package temperature in °C. Pure-C local scan of `/sys/class/hwmon` for known CPU drivers (`coretemp`, `k10temp`, `k8temp`, `zenpower`, `cpu_thermal`/`cpu-thermal`), reading every `tempN_input` (millidegrees → °C); only if hwmon yields nothing does it fall back to `/sys/class/thermal` zones with CPU `type`s (`x86_pkg_temp`, `coretemp`, …) to avoid counting the same package twice (`pn-temp.c:181-206, 385-400`).
**Settings** — `hostname`; `aggregation` (enum `PnTempAggregation`: `average` default / `maximum`, `pn-temp.c:869-878`); inherited `period` (default 5).
**Emits** — `value` (avg or hottest), plus `average`, `min`, `max`, `count` (int), `unit`="°C", `hot` (hottest sensor label), `aggregation`, and **`readings`** — a JSON array of `{label, value}` for every sensor sampled (`pn-temp.c:683-733`).
**Gotchas** — **A successful sample with zero matched sensors is forced to `success=FALSE`** with reason "no CPU temperature sensors found" rather than emitting a misleading 0.0 (`pn-temp.c:680-681, 750`). Remote sampling ships an embedded `sh` script over SSH that emits `name|label|millidegrees` lines (`pn-temp.c:412-447`); the unit-test seams `pn_temp_parse_remote_lines`/`pn_temp_aggregate` are public.

### Ambient Temperature
**Purpose** — Chassis/motherboard/chipset (room-ish) temperature — structurally identical to CPU Temperature but with a *different sensor filter*: local hwmon match is restricted to `acpitz`; the thermal-zone fallback covers `acpitz`, `pch_*` (chipset), Intel DPTF participant sensors (`INT340[1-9] Thermal`), `gen_thermal`, `TZ00`/`TZ01` (`pn-ambient.c:183-215, 415-455`). Distinct icon (`fa-thermometer-quarter`, low mercury) to read cooler than CPU Temperature.
**Settings** — `hostname`; `aggregation` (enum `PnAmbientAggregation`: `average` default / `maximum`); inherited `period` (default 5).
**Emits** — identical field set to CPU Temperature (`value`/`average`/`min`/`max`/`count`/`unit`/`hot`/`readings`/`aggregation`).
**Gotchas** — Same zero-sensors → `success=FALSE` ("no ambient temperature sensors found", `pn-ambient.c:737`). Super-IO chips (nct67xx, it87) are intentionally *not* matched by name to avoid polluting the average with CPU/fan-tach readings — such boards rely on the thermal-zone fallback (`pn-ambient.c:173-189`).

### Disk I/O
**Purpose** — Disk throughput (bytes/sec → configured unit) from the per-tick **delta** of `/proc/diskstats` cumulative sector counters (sectors are always 512 B, `pn-disk-io.c:38-42`). Sums all whole disks by default, skipping pseudo-devices (`loop`/`ram`/`zram`/`sr`/`fd`) and partitions so a parent disk is not double-counted through its partitions (`pn-disk-io.c:274-342`).
**Settings** — `hostname`; `device` (string, default `""`=all; e.g. `"sda"`, `"nvme0n1"`, `"dm-0"`); `unit` (enum `PnDiskIoUnit`, **default GByte/sec**, decimal 1 KByte=1000 B, `pn-disk-io.c:838-846`); inherited `period` (default 2).
**Emits** — `value` (total rate), `read`, `write`, `unit`, and `device` (the matched label, `+`-joined when summing).
**Gotchas** — **Same warm-up-on-first-tick behaviour as CPU** (zero reading + "warming up" output, `pn-disk-io.c:666-686`); host/device change resets the baseline. Negative deltas (counter reset / remote reboot) clamp to 0 rather than spike (`pn-disk-io.c:632-633`). A device name with no matching row → `success=FALSE` ("device not present in /proc/diskstats"). Public test seam `pn_disk_io_parse_diskstats`.

### Network I/O
**Purpose** — Network throughput from the per-tick **delta** of `/proc/net/dev` RX/TX byte counters (`pn-net-io.c:274-399`). Sums all interfaces by default, always skipping loopback (`lo`).
**Settings** — `hostname`; `interface` (string, default `""`=all non-loopback; e.g. `"eth0"`, `"wlan0"`, `"enp86s0"`); `unit` (enum `PnNetIoUnit`, **default MByte/sec**, decimal, `pn-net-io.c:737-748`); inherited `period` (default 2).
**Emits** — `value` (total), `rx`, `tx`, `unit`, `interface` (matched label).
**Gotchas** — Throughput is in **bytes**/sec (kernel counter); **multiply by 8 downstream for Mbps/Gbps** (noted in the `unit` pspec doc, `pn-net-io.c:743-745`). Same warm-up first tick (`pn-net-io.c:571-587`) and 0-clamp on negative deltas. Unknown interface → `success=FALSE` ("interface not present in /proc/net/dev"). Public test seam `pn_net_io_parse_net_dev`.

### Net Connections
**Purpose** — Count of **ESTABLISHED** TCP sockets (kernel hex state `01`), across **every network namespace** the probe can see — so LXC/Docker containers are included when run as root (`pn-connections.c:32-76`). Counts IPv4 and IPv6 (`/proc/<pid>/net/tcp` + `tcp6`).
**When to use** — Track live connection load on a host or container fleet box.
**Settings** — `hostname` (doc notes container coverage needs root, `pn-connections.c:432-442`); inherited `period` (default 5). No other settings.
**Emits** — `value` = the count (**int**, via `pn_message_set_int`), plus `host`/`success`/`output` ("Host `<host>` has `<n>` connections."). No breakdown fields.
**Gotchas** — Unlike the temp/`/proc`-parsing nodes, this one **runs a shell script** (`sh -c`) even locally — it dedupes processes by netns inode (`stat -L` on `/proc/<pid>/ns/net`), then `awk`-counts state-`01` rows in a fixed number of forks; so it depends on a POSIX shell + coreutils + awk being present (diverges from the "pure C, no subprocess" pattern the other nodes follow locally). Only ESTABLISHED is counted — TIME_WAIT/CLOSE_WAIT/LISTEN are excluded.
