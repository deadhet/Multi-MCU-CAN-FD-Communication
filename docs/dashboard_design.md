# Monitoring dashboard: design

Covers `dashboard/noccarc_dashboard/` (`link.py`, `model.py`, `server.py`, `web/`). The UART link it reads is MCU1's `monitor/` (Design Document §8).

## Running it

| | Command (from `dashboard/`) |
|---|---|
| With Renode (any scenario) | `uv run --frozen python -m noccarc_dashboard.server --port 3456 --lost-ms 1500` |
| With the board | `uv run --frozen python -m noccarc_dashboard.server --serial /dev/ttyUSB0` |
| Demo (from the repo root) | `renode/demo.sh`: opens the dashboard, then runs `renode/demo.resc` (about 2.5 min); `renode/demo.sh s2` runs scenario S2 instead (s1-s4) |
| By hand, one terminal per MCU | `renode/network.sh`, then `renode/mcu.sh 1` … `4` (see `docs/running_in_renode.md`) |

The page opens at `http://127.0.0.1:8765/` (`--http-port` to change, `--no-browser` to not open it). `--lost-ms 1500` for Renode: it runs at about 0.3× real time on the development PC, so a snapshot reaches the PC only every ≈0.4 s.

## Technology (Het, 13 Nov 2026)

A web page served by Python's standard library, chosen over Tkinter (the assignment allows "Python GUI or web-based interface"). No new dependencies.

```
MCU1 USART3 ─► link.py (thread: bytes → frames → LinkCore) ─► Link.view()
                                                                  │
                         model.py: every text and state level ◄───┘
                                     │  JSON
server.py (127.0.0.1:8765) ─ /stream (Server-Sent Events, 5 per second), /initial.js
                                     │
                         web/app.js draws it into web/index.html + style.css
```

- **Server-Sent Events**: one long HTTP response, one `data: {json}` message every 200 ms. Data only flows one way (MCU1 → PC → page); the browser reconnects by itself.
- **All decisions in Python** (`model.py`, tested by pytest); `app.js` only draws. Text from the device is set with `textContent`, never as HTML.
- **No build step and no external files**: three static files, system fonts, works offline.

## Page: a header and exactly three sections

The assignment requires exactly three sections (§9). The header is the page's status line, as in the assignment's own sample, not a section.

| Assignment item | Where | Source |
|---|---|---|
| Online controllers out of 4 | Section 1, "Controllers online" + one box per MCU | snapshot `online_count`, `nodes[].link` |
| Synchronized parameters out of 20 | Section 1, "Parameters synchronized" + one box per parameter | snapshot `synced_count`, `params[]` |
| Network health with a defined calculation | Section 1: percentage, bar, the formula with both counts; header label | snapshot `health_percent` (computed on MCU1) |
| Arbitration and data bit rate, BRS | Section 1, CAN FD configuration | CONFIG |
| MCU identification | Section 2, MCU | |
| Online/offline status | Section 2, Status (diag/'s link state) | `nodes[].link` |
| Heartbeat reception status | Section 2, Heartbeat | from the link state |
| Last heartbeat age | Section 2, Last heartbeat | `heartbeat_age_ms` |
| Missed heartbeat count | Section 2, Missed | `missed` |
| Communication timeout status | Section 2, Communication timeout | from the link state |
| Recovery and re-synchronization status | Section 2, Recovery / re-sync; restarts; the event log | link state, `reported_state`, `restarts`, EVENT frames |
| P1–P20: ID, value, source MCU, freshness, validity | Section 3 (plus name, type, age) | snapshot `params[]`, CONFIG names |
| Stale or invalid visually distinguishable | Section 3: amber STALE, red INVALID, red "MCU n offline" | |
| Updates dynamically | 5 times a second; MCU1 sends a snapshot every 100 ms and an event on every change | |
| Must not generate or substitute values | Every value is from MCU1's frames. Before the first snapshot: "Waiting for MCU1…". Link lost: the last data stays, greyed, with "LOST: no valid frame for n s" | |
| UART communication-loss handling | Header: CONNECTED / LOST (no valid frame for `lost_ms`) / DISCONNECTED (no source, retried every second); Section 1: frames received, lost, replaced, frame errors, MCU1 restarts | `link.py` |

## Display rules

Style of a high-performance HMI (after ANSI/ISA-101, operator displays in process plants), chosen also to avoid the look of generated web dashboards (gradients, glass, rounded cards, donut charts, green pills everywhere):

- Grey surfaces. A healthy network shows **no colour**: ONLINE, FRESH, VALID are plain text.
- Colour only for abnormal states, never colour alone: **amber ▲ caution**, **red ■ fault**, **blue ● in transition**, always with the state's word.
- One strong mark per fault (the state column); columns derived from it, and the event log, get a small box only.
- Numbers in a monospace face with tabular figures, right-aligned, with units.
- All colours are tokens in `style.css` (`:root`); nothing else names a colour.

| State | Level | Shown |
|---|---|---|
| ONLINE, FRESH, VALID, OPERATIONAL | normal | plain text |
| LATE, NO_HEARTBEAT, STALE, missed 1–2, DEGRADED | caution | amber ▲ |
| OFFLINE, DATA_INVALID, INVALID, owner offline, missed ≥ 3, DOWN, link LOST / DISCONNECTED | fault | red ■ |
| RECOVERING, UNKNOWN (not heard yet), BOOT, RESYNCING, MCU1 restarted | in transition | blue ● |
| NEVER RECEIVED, n/a, no data | none | grey text |

Health label (Het, 13 Nov 2026): 100 % HEALTHY, 1–99 % DEGRADED, 0 % DOWN; NO DATA while the link is not CONNECTED.

Controllers online (Het, 13 Nov 2026): MCU1 plus every peer that is ONLINE or **LATE**. One late heartbeat is not yet a failure, so health does not dip for it; OFFLINE, RECOVERING, NO_HEARTBEAT and DATA_INVALID do not count. The rule is in the firmware (`monitor_online_count()`); the dashboard shows MCU1's figure.

### Section 2 columns from diag/'s link state

| Link state | Heartbeat | Communication timeout | Recovery / re-sync |
|---|---|---|---|
| ONLINE | Receiving | None | from the reported state: Normal (OPERATIONAL), Re-synchronizing (RESYNCING), Booting (BOOT) |
| LATE | Late | Heartbeat late | as ONLINE |
| NO_HEARTBEAT | Not received | Heartbeat lost, data arriving | as ONLINE |
| DATA_INVALID | Receiving | No valid data block | as ONLINE |
| OFFLINE | Not received | Heartbeat and data | Waiting for heartbeat |
| RECOVERING | Receiving | None | Recovering: waiting for OPERATIONAL |
| UNKNOWN | Not heard yet | Not heard yet | Not heard yet |

MCU1's own row: "This node"; its link to the PC is the header's UART link. Its restarts are those the PC saw (sequence back to 1).

The event log (Section 2) is newest first, with MCU1's time and the PC's time. MCU1's times start again at 0 when it restarts; a separator row marks that.

## Safety

- Listens on 127.0.0.1 only; nothing on the network can reach it.
- Serves exactly `index.html`, `style.css`, `app.js`, `/initial.js` and `/stream`; never a file by name (`web/mock.js`, sample data for layout review, is never served).
- Answers only when the `Host` header is this PC (against DNS rebinding).
- `Content-Security-Policy: default-src 'self'`, `nosniff`, `no-referrer`, `no-store`.
- Read-only: nothing on the page can send anything to MCU1.

## Tests

- `dashboard/tests/test_model.py` (39): a healthy network shows no colour at all; before the first snapshot nothing is shown; a lost link keeps the data, greyed; health labels; every link state's Section 2 columns; missed levels; values by type and float formatting; parameter states; CONFIG and UART rows; events, their words and levels, the restart separator, the cap; device text passed as text.
- `dashboard/tests/test_server.py` (10): the three files with the safety headers; the initial model; nothing else from the disk (`mock.js`, `..`, encoded `..`); 403 for another Host; the stream's first message; 127.0.0.1 only.
- Mutants, each caught: LATE shown as normal; no Host check; serving any file in `web/`.
- Live, Renode S3 with the server (13 Nov 2026): DISCONNECTED → CONNECTED → 100 % HEALTHY → MCU3 LATE (75 %) → OFFLINE → RECOVERING → 100 % → Renode quit: DISCONNECTED, last data greyed. (Measured before LATE counted as online: with the 13 Nov rule, LATE keeps 100 % and health drops to 75 % at OFFLINE.)
- Live, `renode/demo.sh`: every state of `demo.resc` seen in order, including the link LOST while MCU1 was halted and MCU1's restart (1 seen), 0 frames lost, 0 frame errors.

## Future scope

Trend lines per parameter; acknowledging alarms (blinking until acknowledged, as in ISA-18.2); a dark control-room theme; saving the event log to a file; the board's serial port chosen from a list.
