# HA state accuracy & bus-friendly comms

<!-- Goal plan. Managed by agents via the `goals` tool and by hand.
     Design: DESIGN.md (sibling file). Format: header block, then
     `## <phase title>` + `Status:` + `- [ ] task` checkboxes. -->

Status: active
Order: 999

Fix fan-speed state never reaching HA, slow/incorrect state after HA-originated changes, and reduce/shape RC-EX3 bus traffic. Protocol is reverse-engineered — hardware-dependent hypotheses must be confirmed with logs before code changes.

## 1. Confirmed code bugs (no hardware needed)
Status: done

Bugs provable from code + ESPHome 2026.9 climate API. See DESIGN.md §Findings A.

- [x] Publish fan speed via set_custom_fan_mode_()/set_fan_mode_(CLIMATE_FAN_AUTO) in parse_status_response and control(); drop requested_custom_fan_mode_ and the CLIMATE_FAN_ON assignments
- [x] Selecting fan "Auto" in HA after a numbered speed must send 0x07 (currently the stale custom speed is re-sent)
- [x] Turning OFF from HA must not rewrite the unit's mode to auto (currently sends mode=00); remember last on-mode and send it (or FF once phase 2 confirms FF = unchanged)
- [x] Bound the RSR2 → RSR2 echo loop (max retries per cycle) so a not-ready unit can't hold the bus
- [x] Update PROJECT.md drift (apply_wire_fan_mode, 30s default vs 5min yaml, return-air /4 vs /10, "op-data always requested") and bump YAML pin

## 2. Hardware evidence capture
Status: planned

Answer protocol questions with VERBOSE rx logs on the real unit before changing timing/protocol behaviour. See DESIGN.md §Open questions.

- [x] Enable logger VERBOSE for rc_ex3 (logs: level VERBOSE / logs: rc_ex3: VERBOSE) and log every TX frame too
- [x] Q1: What does the unit reply to an RSSL13 set command (ack? echo? pre-change status?) and how long until a status poll reflects the change
- [x] Q2: Does FF in an RSSL13 field mean "leave unchanged" (e.g. send only temp)
- [ ] Q3: How long does the panel stay in "communicating" lock-out after one request/response, and what does rapid back-to-back RSSL13 do (dropped? garbled?)
- [x] Q4: Time from boot to first status publish (PollingComponent first update timing with 5min interval)
- [ ] Q3 test: HA script calling set_hvac_mode then set_temperature back-to-back, watch VERBOSE TX/RX log for both commands being applied

## 3. State sync after HA commands
Status: planned

Make HA reflect the unit's real state quickly after HA-originated changes, with minimal extra bus traffic. See DESIGN.md §Findings B.

- [ ] Single confirm status poll a few seconds after each sent command (delay from Q1/Q3); reset the regular poll timer so it doesn't add traffic
- [x] Ignore/stale-guard status responses to requests sent before a pending command so they can't revert the optimistic state
- [x] Poll once shortly after boot (don't wait for the first 5min interval)

## 4. Bus traffic shaping
Status: planned

Keep the panel usable: one outstanding request at a time, bounded retries, no redundant writes. See DESIGN.md §Findings C.

- [x] TX gate: one outstanding request at a time with response timeout; queue control/status/op-data behind it
- [ ] If Q2 confirms, send FF for unchanged fields in RSSL13 so HA can't clobber a concurrent panel change
- [ ] If Q3 shows back-to-back RSSL13 is dropped/garbled: enforce a minimum gap after each command reply before the next command (changes arriving in the gap merge into it)
- [ ] Skip op-data when no diagnostic sensors are configured; make confirm-poll delay (and inter-command gap, if added) YAML-configurable
