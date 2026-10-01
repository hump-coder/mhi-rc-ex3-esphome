# HA state accuracy & bus-friendly comms

Investigation of three user-reported symptoms against `components/rc_ex3/` at
`b1a6b9e`, checked against the ESPHome 2026.9.1 climate API
(`esphome/components/climate/climate.{h,cpp}`).

Reported symptoms:
1. Fan speed in HA is blank unless it was set from HA in the current browser session.
2. After an HA-originated change, HA takes a long time to show the real state.
3. The panel dislikes heavy traffic. While we talk to it, the physical panel is
   locked out ("communicating"), so we can't simply poll faster.

The protocol is reverse-engineered, not documented. Anything marked
**hypothesis** needs hardware logs (phase 2) before we build on it.

## Findings A: bugs confirmed in the code

### A1. Fan speed is never reported to HA (symptom 1, root cause)
Commit `08e68de` ("Fix custom fan mode handling for updated ESPHome API")
replaced writes to the climate's custom fan mode with a private
`std::string requested_custom_fan_mode_`. In ESPHome ≥2025.11 the published
custom fan mode lives in `Climate::custom_fan_mode_`. It is set only through the
protected `set_custom_fan_mode_()`, and `set_fan_mode_()` clears it.
`publish_state()` never looks at `requested_custom_fan_mode_`.

As a result:
- `parse_status_response()` sets `fan_mode = CLIMATE_FAN_ON` for speeds 1–4.
  `ON` isn't in the supported fan modes (only `AUTO` is), and no custom fan
  mode is set, so HA gets a fan mode it can't show and displays blank.
- `control()` does the same. The value is "visible in session" only because the
  HA frontend keeps showing the option the user just picked. Reloading the page
  shows blank again.

Fix: in both places call `set_custom_fan_mode_("1".."4")` for speeds 1–4, and
`set_fan_mode_(CLIMATE_FAN_AUTO)` (which clears the custom mode) for auto.
In `control()`, read `call.get_fan_mode()` and `call.get_custom_fan_mode()`.
Don't keep private shadow state.

### A2. Picking "Auto" from HA after a numbered speed does nothing
`control()` only changes `requested_custom_fan_mode_` when a custom mode
arrives. A call carrying `fan_mode = AUTO` leaves the stale speed in place, so
the old speed is sent again. The A1 fix resolves this as well.

### A3. Turning off from HA resets the unit's mode to auto
With `mode == OFF`, `climate_mode_to_wire()` falls through to `default: return 0`
(auto), and the packet carries `power=00 mode=00`. When the panel later turns
the unit on, it starts in auto rather than the mode it was in before. Fix:
remember the last "on" mode from status and send that, or send `FF` for the mode
once Q2 is answered.

### A4. The RSR2 echo loop has no limit
Every `RSR2` reply immediately sends `RSR20000E9` again, with no counter. A unit
that keeps answering "not ready" keeps the bus busy (and the panel locked) with
no end.

### A5. PROJECT.md is out of date
It describes `apply_wire_fan_mode()` (removed), a "default 30 s" poll (the YAML
uses 5 min), return-air `/4` (the code uses `/10` since `b1a6b9e`), and says
op-data is "always requested" (it's gated by `op_data_interval`).

### Phase 1 outcome (commits e120a1e, 3d8742e, 0ebde0f, 306d0f0)
- A1/A2: fixed as described. The wire fan value now comes from
  `get_custom_fan_mode()` after the call is applied.
- A3: `last_on_mode_` is updated from status (when on) and from HA (non-OFF
  modes). Until the first status arrives it defaults to auto, so an HA
  power-off sent before any poll still sends `mode=00`. Switch to `FF` if Q2
  confirms it means "unchanged".
- A4: `MAX_RSR2_RETRIES = 5`. The counter resets on `RSR1` or a new page-1
  request. The value 5 is a guess; tune it with phase 2 logs.
- Verification: a host `g++ -fsyntax-only` check against the ESPHome 2026.9.1
  headers. A full ESP32 build couldn't run on the dev machine (no
  `python3-venv`/`ensurepip`). Hardware testing is still needed.

### Checked and fine
- The hard-coded checksums are correct: `RSSL12…43FF` → `25`, `RSR10000` → `E8`,
  `RSR20000` → `E9`.
- Field offsets in the `RSSL13` TX template match the RX parse offsets
  (13/17/21/30–31).

## Findings B: slow state after HA changes (symptom 2)
Code facts:
- `control()` sends one `RSSL13` and publishes the requested state
  optimistically. Nothing reads the state back afterwards. The next status read
  is the regular poll, up to `update_interval` (5 min) later.
- Every reply starting `RSSL1` goes to `parse_status_response()`. If the unit's
  reply to `RSSL13` (or a poll reply that crosses the command on the wire)
  carries the state *before* the change, it overwrites the optimistic state.
  HA then shows the old values until the next poll, up to 5 min later.
  **Hypothesis:** this matches the symptom closely. Q1 confirms or rules it out.
- HA's +/- temperature buttons send several `control()` calls in quick
  succession, each becoming a full `RSSL13` packet sent straight away. If the
  panel drops some of them (**hypothesis**, Q3), the unit ends up on an
  intermediate value while HA shows the last one.
- Separately, A1 always makes the fan field look wrong.
- After boot, nothing is published until the first poll runs (Q4). HA shows
  the defaults set in `setup()` (OFF / 22 °C) until then.

Planned direction (phase 3):
- ~~Merge rapid calls into one send after a ~1 s quiet period.~~ Dropped
  (2026-10-01). Hardware logs show a burst of +/- clicks in HA producing a
  single `RSSL13`. The HA frontend debounces the thermostat controls before
  calling `climate.set_temperature`. The TX gate also builds the command at
  send time, so changes that arrive while a command is queued merge into it.
  A device-side debounce would add latency to every command for no gain.
  What's left is non-debounced callers (automations/scripts/API) making two
  service calls back-to-back. The second command then goes out right after the
  first one's reply. That's a Q3 question. If Q3 shows the panel mishandles
  it, phase 4 adds a minimum gap after each command reply instead.
- Do one confirmation poll a few seconds after each send, and reschedule the
  regular poll so there's no net extra traffic.
- Ignore status replies to requests sent before a pending command.
- Poll once shortly after boot.

## Findings C: bus traffic (symptom 3)
- No TX arbitration: `control()`, `update()` and the op-data chain in `loop()`
  all write whenever they like. A control packet can land in the middle of a
  status reply.
- Each poll cycle can send status → op-data → (RSR2 → RSR2 …).
- Every control packet overwrites all four fields. If the panel was changed in
  the meantime, HA stomps on that change.

Planned direction (phase 4):
- Allow one outstanding request at a time, with a timeout.
- Bounded retries.
- Send `FF` ("no change") for power/mode/fan that HA didn't touch (Q2: confirmed;
  the setpoint must always be sent).
- Skip op-data when no diagnostic sensors are configured.

## Open questions (phase 2: answer with VERBOSE logs on hardware)
- **Q1:** What does the unit reply to `RSSL13`? An ack, an echo of the command, or
  the status from before the change? How long until a status poll shows the new value?
- **Q2:** Is `FF` in an `RSSL13` field treated as "leave unchanged"? The status
  query uses `FF` for every field, which suggests it might be.
- **Q3:** How long does the panel's "communicating" lock-out last after one
  exchange? What happens to back-to-back `RSSL13` packets?
- **Q4:** How long after boot is state first published with `update_interval: 5min`?

Logging needed: set `logger: logs: rc_ex3: VERBOSE` (RX is already at `LOGV`),
and add a `LOGV` for every TX frame, with timestamps, before running the tests.

### Q2 answer (2026-10-01, commit 489798e test hook)
Tested with `send_raw_command()` from an api action, unit off / cool / fan 3:
- `FF` for power/mode/fan with setpoint `05032E` → only the setpoint changed;
  power `FF` did not turn the unit on.
- Fan-only (`0301`) and mode-only (`0303`) with setpoint `05032E` → only that
  field changed. Mode changes while off are applied.
- The same fan-only / mode-only / power-off commands with `05FF` for the
  setpoint → nothing applied: answered `RSSL08FF0040{13,12}32540100` or a normal
  `RSSL11` ack, and once the next status poll also got `RSSL08`. `RSSL08` is
  probably a rejection, not the "applied anyway" ack PROJECT.md used to say.

Consequence for phase 4: send `FF` for power/mode/fan that HA didn't change,
but always send the setpoint. A panel setpoint change between polls can still
be overwritten; panel power/mode/fan changes can't. Power-off can send mode
`FF` instead of `last_on_mode_`, removing the pre-first-poll `mode=00` case.

### Phase 4: FF for fields HA didn't change
`control()` records which fields the call touched (`cmd_fields_`): a mode
change sets power, and also sets mode unless the new mode is OFF; any fan or
custom fan change sets fan. `send_command_()` sends `FF` for every unset
power/mode/fan field, and always sends the setpoint. Changes merged into a
queued command add their fields to it. A timed-out command's fields
(`cmd_inflight_fields_`) carry over to its resend, or to the newer queued
command that replaces it. Power-off is now `pwr=00 mode=FF`, so
`last_on_mode_` is gone, along with the pre-first-poll `mode=00` case (A3).
Hex fields are now uppercase, matching the Q2 raw tests and the status query.
Limitation: a panel setpoint change made since the last poll is still
overwritten by any HA command.

### Phase 3: confirmation poll without extra traffic
The confirming poll (`CMD_CONFIRM_DELAY_MS` = 2 s after each command) was
already in place. To stop it adding traffic, `update()` skips its regular
status poll when a confirmation is still due (`command_pending_` /
`cmd_confirm_pending_`) or a status reply was applied less than half an
`update_interval` ago. The state is therefore never more than ~1.5 intervals
old. If op-data is due on a skipped cycle, it is queued directly. When a
confirmation is due, op-data is chained to the confirmation's reply instead,
so the confirmation isn't held up behind the ~40 s handshake. Restarting
ESPHome's poller was rejected: frequent commands would keep postponing
`update()` and starve op-data. The 2 s delay stays a placeholder until Q3.
