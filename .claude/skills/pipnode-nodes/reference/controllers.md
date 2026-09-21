# Controllers — whole appliances in one node

The **Controllers** palette group holds self-contained state machines that run a whole appliance rather than transform one message. Wire the inputs and outputs up and the node *is* the thing. They are GTK-free core nodes (`lib/pn-*.c`) with no `-gui.c` companion — the header is the whole node, and every setting renders in the generic settings dialog.

What sets them apart from the **Filters** groups: several inputs that mean *different things* (not several operands of one sum), several outputs that speak at *different times*, and state that lives across messages and is driven by an internal clock as well as by arrivals.

Currently one node: **Burglar Alarm**.

## Burglar Alarm

**Purpose** — The panel of an intruder alarm: the box on the hall wall that watches four zones, counts the exit and entry delays, and decides when the siren goes off. (`lib/pn-burglar-alarm.c`)

**When to use** — Any arm/disarm + zones + delay + siren shape, not only a house: a machine-room door watch, a freezer-lid monitor, a "did anything move in the shed" pager. It is to a **Keypad** what **Calculator Engine** is — the state machine behind the keys — only with five inputs and five outputs.

**Ports** — 5 inputs (use `target_input`): 0 `keypad`, 1 `delayed`, 2 `zone 1`, 3 `zone 2`, 4 `zone 3`. 5 outputs (use `source_output`): 0 `alarm`, 1 `display`, 2 `armed`, 3 `speech`, 4 `sms`.

**Settings**
- `code` (string, default `"1234"`) — the code that arms and disarms. Empty means the panel has no code: a bare `#` arms and disarms. **Stored in the worksheet as plain text.**
- `exit-delay` (uint, 0–600, default `30`) — seconds from an accepted code to actually armed. `0` arms at once.
- `entry-delay` (uint, 0–600, default `30`) — seconds from the delayed zone tripping to the siren. `0` makes the delayed zone instant.
- `siren-time` (uint, 0–3600, default `180`) — seconds the siren sounds before it cuts off. `0` = ring until disarmed.
- `display-lines` (int, 2–4, default `2`) — how many lines the `display` output carries. Set it to the line count of the **Matrix57** LCD it feeds.
- `sms-on-arming` (bool, default `FALSE`) — also text on every arm/disarm. Alarms and their clearing always go out.

**Reads**
- `keypad`: `data.key` (a string) is one keystroke — what **Keypad** on its `Decimal Keyboard` layout emits. Digits build the code, `#` (or `=`) submits, `*` / `C` / `CE` rub out. Failing that, a `data.output` string is taken as a **whole code submitted at once**, so MQTT or a **Text** node can arm remotely. Anything else on this input is ignored in silence.
- zones: a numeric `data.value` sets the zone's **held** state (`> 0.5` open) — a door contact. **No** numeric value is a **momentary** trip that leaves the zone closed — a PIR. The difference decides whether the panel can arm.

**Writes** — every message carries `data.state` (`disarmed` / `exit` / `armed` / `entry` / `alarm`) and `data.success = TRUE`.

| Output | `data.value` | `data.output` | also |
|---|---|---|---|
| `alarm` | 1.0 / 0.0 (siren) | `ALARM: <zone>` / `OK` | `data.cause` = the zone name |
| `display` | countdown seconds (0 when idle) | 2–4 newline-separated lines | — |
| `armed` | 1.0 / 0.0 | `ARMED` / `DISARMED` | — |
| `speech` | the state's number 0…4 | one spoken sentence | `data.cause` |
| `sms` | the state's number 0…4 | one written sentence | `data.cause` |

`alarm` and `armed` are written **only on a change**; `display` on every event and once a second while a delay runs; `speech` and `sms` only when there is something to say.

**States** — `disarmed` → (valid code, and no *instant* zone held open) → `exit` → `armed` → (delayed zone) → `entry` → `alarm`; a valid code returns to `disarmed` from anywhere. An *instant* zone goes straight from `armed` **or** `entry` to `alarm`. Zones are ignored during `exit` — but a zone left open is re-read the moment arming bites, so a window left open during the exit delay trips then. In `alarm` the siren cuts off after `siren-time` while the state stays `alarm` (the panel remembers what tripped it, and paints itself red on the worksheet via the generic node error state) until a code disarms it.

**Zone names are the input names.** The panel announces a zone by `pn_node_get_input_name()`, so renaming an input on the node's **Inputs** tab renames the zone in the readout, the spoken sentence and the sms — "Alarm! Kitchen window."

**Readout** — four lines are built and the first `display-lines` emitted: (1) banner `READY` / `NOT READY` / `EXIT 25` / `ARMED` / `ENTRY 20` / `*** ALARM ***`; (2) detail, or a one-shot complaint (`WRONG CODE`, `NOT READY`) that the next keystroke clears; (3) zone map `Zones D.2.` — an open zone shows its marker (`D`, `1`, `2`, `3`), a closed one a dot; (4) the masked code `Code: ****`. On two lines the masked code displaces the detail while typing.

**Gotchas**
- The state is **not serialized**: a reloaded worksheet comes up disarmed. On load the node announces that once on `alarm`, `armed` and `display` (via a `g_idle` startup announce) so a wired LCD is not left blank and a wired siren is told to be quiet — and stays **silent** on `speech`/`sms`, which would otherwise talk to the room on every open.
- Arming is refused while an **instant** zone is held open (`NOT READY` names it). The **delayed** zone may be open — that is the door being left through.
- A second zone tripping during an alarm updates `data.cause` and restarts the siren, but does **not** send a second sms.
- The logic seam (`pn_burglar_alarm_press` / `_submit` / `_set_zone` / `_trip_zone` / `_tick`) is public and message-free, so a headless test walks a thirty-second delay in thirty calls. `_tick()` deliberately does not touch the node's 1 Hz timer.

**Worked example** — `examples/controls-and-logic/burglar-alarm.json`: Keypad + four Switches → panel → a 20×4 Matrix57, two LEDs (siren, armed), a Text to Speech and a Text View standing in for a Meshtastic node.
