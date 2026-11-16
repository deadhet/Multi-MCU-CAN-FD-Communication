# diag/ module: health, fault detection and recovery

Covers `firmware/common/diag/` and how the apps use it: assignment Stage 3 (heartbeat monitoring and fault detection) and Stage 4 (recovery and re-synchronisation). Section numbers (§) refer to the Design Document. Every node runs diag/: each needs to see its peers' states, and MCU1 additionally reports them (UART link, a later stage).

diag/ only detects and reports; it takes no fault actions. It calls no other service (§4.4, calls only go downward). The app feeds it heartbeats, parameter blocks and each peer's data status, and carries out what it returns. Times are passed in, so the module also runs on a PC.

## What it does

| Job | Where | Design |
|---|---|---|
| Link state per peer: UNKNOWN, ONLINE, LATE, OFFLINE, RECOVERING, NO_HEARTBEAT, DATA_INVALID | `diag_on_heartbeat()`, `diag_on_param_block()`, `diag_update()` | §7.2, plus NO_HEARTBEAT and DATA_INVALID |
| Missed heartbeat count per peer, from the time since its last heartbeat | `diag_update()` | §7.2 |
| Restart count per peer: comm/'s restart flag, or the uptime went down and that was not a wrap past 2^32 (49.7 days) | `diag_on_heartbeat()` | §7.3 |
| Recovery count per peer (RECOVERING → ONLINE) | `diag_on_heartbeat()` | §7.2 |
| Own state BOOT, RESYNCING, OPERATIONAL, sent in this node's heartbeat | `diag_own_state()` | §7.4, with the rules below |
| One log line per change; a summary line every second | `log_peer()`, `diag_debug_report()` | §8.6 |
| Copy of a peer's state for monitor/ (GUI, later) | `diag_get_peer()` | §8.3 |

## Peer link states

| State | Entered when | Left when |
|---|---|---|
| UNKNOWN | Boot | First heartbeat → ONLINE; 350 ms after boot without one → OFFLINE (or NO_HEARTBEAT) |
| ONLINE | A heartbeat from UNKNOWN, LATE or NO_HEARTBEAT; a confirmed recovery; an accepted block while DATA_INVALID | First heartbeat missed (150 ms) → LATE; a restart → RECOVERING; no accepted block for 350 ms → DATA_INVALID |
| LATE | ONLINE or DATA_INVALID, and 150 ms without a heartbeat | Heartbeat → ONLINE; 3 missed (350 ms) → OFFLINE or NO_HEARTBEAT |
| OFFLINE | 3 heartbeats missed and its parameters not all VALID and FRESH | Heartbeat → RECOVERING; its parameters arriving properly again → NO_HEARTBEAT |
| RECOVERING | The first heartbeat after OFFLINE; a restart | A **later** heartbeat that reports OPERATIONAL → ONLINE (recovered); 3 missed → OFFLINE or NO_HEARTBEAT |
| NO_HEARTBEAT | 3 heartbeats missed, but all 5 of its parameters VALID and FRESH here | Heartbeat → ONLINE (RECOVERING if it was OFFLINE before); its data failing too → OFFLINE |
| DATA_INVALID | ONLINE, heartbeats arriving, but no parameter block from it passed checks 1–7 for 350 ms | An accepted block → ONLINE (a heartbeat does not end it); 150 ms without a heartbeat → LATE; a restart → RECOVERING |

**Timing** (§7.2, §10). A heartbeat is due every 100 ms. The missed count is computed from the silence since the last heartbeat, or since boot if none came:

| Silence (ms) | 0–149 | 150–249 | 250–349 | 350–449 | … |
|---|---|---|---|---|---|
| Missed | 0 | 1 (LATE) | 2 | 3 (OFFLINE or NO_HEARTBEAT) | 4 … |

The first heartbeat counts as missed at 150 ms (1.5 × period absorbs the ≈2 ms worst-case delay), each further one 100 ms later. One lost heartbeat therefore makes a node LATE but never OFFLINE. diag/ runs every 5 ms, so a change is seen 0–4 ms after its boundary. The count resets to 0 on every heartbeat.

**The first heartbeat after OFFLINE only means RECOVERING**. It can never also confirm the recovery, even if it says OPERATIONAL. After a reconnect, the first heartbeat may be an old one left in the sender's Tx FIFO: it was sent before the sender lost the bus and still reports the state from then (the same cause as Design Document open item 2, which the settle rule handles for parameters). Only a later heartbeat that reports OPERATIONAL makes the peer ONLINE. The same applies after a restart, whose first heartbeat says BOOT anyway.

**NO_HEARTBEAT**. A peer whose heartbeats stopped but whose parameters still arrive VALID and FRESH (`params_node_synced()`) is alive, with only its heartbeat path failing. It is reported as NO_HEARTBEAT, not OFFLINE: it is not forgotten in comm/ and its parameters do not get the owner-offline flag. A peer whose heartbeats are rejected (bad CRC, for example) counts as not heard: rejected frames never reach diag/.

**DATA_INVALID**, the mirror image of NO_HEARTBEAT. A peer whose heartbeats arrive but from which no parameter block has passed checks 1–7 for 350 ms (its blocks are rejected, for example a bad CRC, or missing) is alive with a broken data path. Without this state, every node that hears it would wait for its data forever and stay RESYNCING. A DATA_INVALID peer is not waited for, so the others go OPERATIONAL with the parameters they do have (`synced=15`). It is not OFFLINE: its heartbeats arrive, it is not forgotten in comm/ and its parameters do not get the owner-offline flag (they are NEVER_RECEIVED or STALE in params/ anyway). Only an accepted block makes it ONLINE again; its heartbeats do not, or it would flap on every heartbeat. If its heartbeats stop too, it goes LATE and then OFFLINE. 350 ms is the same verdict time as OFFLINE: blocks come every 10 ms, so one short burst of bad frames never changes the node's state. Values that pass checks 1–7 but fail check 8 are a parameter matter (INVALID in params/), not a node state.

**Restarts** (§7.3). comm/ flags a restart (option B) only while it still tracks the sender. When a peer went OFFLINE, comm/ has forgotten it, so a reset during the outage (scenario S2) carries no flag. diag/ therefore also counts a restart when a peer's uptime goes down. A halted node that resumes keeps a rising uptime, so it is not counted.

**Uptime wrap**. The uptime is a uint32 ms count: every 49.7 days it passes 2^32 and starts again at 0, which also makes it "go down". A wrap moves the count forward, through 2^32, by about the time that passed at the receiver since the previous heartbeat; a restart leaves it far from that. So a lower uptime counts as a restart only if that forward step exceeds the time passed by more than `UPTIME_SLACK_MS` (1000 ms). Without a restart the two differ only by clock rounding (≤ 2 ms), by how much later the previous heartbeat arrived after it was made (under ≈350 ms while the peer stays connected), and by crystal drift (≤ 100 ppm). Smaller slack risks counting a real wrap as a restart; larger slack lets a reset that lands within the slack before the peer's own wrap pass as a wrap. Both are limited to the slack around the wrap, once in 49.7 days, and after an outage the peer goes RECOVERING either way, so only the count would be off.

The check runs only when the uptime went down. Comparing every heartbeat's step with the time passed was considered and rejected: it would count a false restart after every reconnect, because an old heartbeat left in the sender's Tx FIFO arrives late (S3, `stage6_old_hb_then_boot.resc`), and the next, fresh heartbeat then steps far ahead of the time passed since that late arrival.

## Own state

| Own state | Entered when | Left when |
|---|---|---|
| BOOT | Reset. The first heartbeat, sent at 0 ms, is always BOOT | A heartbeat from any other node → RESYNCING |
| RESYNCING | (a) After BOOT. (b) OPERATIONAL, and no peer connected any more | Every connected peer has sent a parameter block that comm/ accepted since it was last OFFLINE → OPERATIONAL |
| OPERATIONAL | As above | No peer connected → RESYNCING |

Connected = ONLINE, LATE, RECOVERING or NO_HEARTBEAT. A DATA_INVALID peer is neither waited for nor counted as connected: no data is exchanged with it. A node alone (all peers OFFLINE or never heard) never reports OPERATIONAL. Other nodes coming and going do not change a node's own state, so one node's recovery never makes the others report RESYNCING. Accepted limit: if the bus split into two groups of at least 2 nodes each, both could report OPERATIONAL; the debug log shows every node's view of the others. In Renode a split is always one node alone, which stays RESYNCING.

This replaces §7.4's "OPERATIONAL when every other node has been decided (ONLINE with valid, fresh parameters, or OFFLINE after 350 ms)".

## How the app uses it

| Where | What |
|---|---|
| `diag_task`, every 5 ms | Asks params/ which peers' data is proper (`params_node_synced()`), calls `diag_update()`, calls `comm_forget_peer()` for each node that went OFFLINE (§5.6: its next frame is accepted whatever its sequence), and copies each peer's OFFLINE status to params/ (`params_set_owner_offline()`) |
| Main loop | Accepted heartbeats → `diag_on_heartbeat()`; accepted parameter blocks → `params_apply_block()` and `diag_on_param_block()`. Rejected frames reach neither |
| `heartbeat_task` | `state = diag_own_state()` |
| `report_task` | `diag_debug_report()` after comm/'s and params/' lines |

## Rejected frames in the log (comm/)

Every frame that fails checks 1–7 is logged by reason, within §8.6 (no per-frame logging): the first one of a reason at once, then at most one line per second with the count since the last line. The 1 s report flushes the rest.

```
MCU1 t=500 rx rejected crc id=50 n=1
MCU1 t=1005 rx rejected crc id=50 n=4
```

Reason names as on the `err` line (`id dupnode fmt len ver src crc dup old`); `id` is the CAN ID of the latest one (decimal, so 50 = 0x032, node 2's heartbeat); `n` counts the rejections since the previous line.

## Logging limits

| Limit | Where | Effect |
|---|---|---|
| Change-only logging (§8.6) | params/ `log_changes()`, diag/ `set_link()`, `set_own()` | One line per change, nothing while a state holds; never per frame |
| Rejected frames: first at once, then at most one line per second per reason | comm/ `reject()` | A node sending 100 bad frames a second costs one line a second, with the count |
| USART1 transmit buffer, 2048 bytes | `drivers/uart.c` | A message that does not fit is dropped whole and counted. The count was never printed; since 12 Nov 2026 the report ends with `MCUn log dropped=N`. At 115200 baud (≈11.5 KB/s) the 1 s report (≈1 KB) fits easily; in Renode USART1 is not paced, so nothing is ever dropped there |
| Counters | comm/, params/ | 32-bit, wrap after 2^32. The heartbeat's 16-bit error summary saturates at 65535 |

## Debug output

Change lines, the moment they happen (MCU1 in scenario S2):

```
MCU1 t=0 diag n2 ONLINE
MCU1 t=0 diag self RESYNCING
MCU1 t=15 diag self OPERATIONAL
MCU1 t=5050 diag n3 LATE
MCU1 t=5250 diag n3 OFFLINE
MCU1 t=5250 db OWNER_OFFLINE 11 12 13 14 15
MCU1 t=10000 diag n3 RESTART
MCU1 t=10000 diag n3 RECOVERING
MCU1 t=10005 db OWNER_ONLINE 11 12 13 14 15
MCU1 t=10100 diag n3 ONLINE recovered
```

Summary, every second after comm/'s and params/' lines:

```
MCU1 diag self=OPERATIONAL n2=ONLINE/OPERATIONAL age=5 miss=0 r=0 rec=0 n3=... n4=...
```

`nX=link/state`: how this node sees node X, then the state X last reported (`-` if never heard). `age`: ms since its last heartbeat (since boot if never). `miss`, `r` (restarts), `rec` (recoveries). params/' `db` line gains `offline=`, the number of parameters with the owner-offline flag. A last line, `MCU1 log dropped=0`, counts debug text that did not fit in the 2 KB USART1 buffer (see "Logging limits").

## Results (Renode 1.17.0, 100 µs quantum)

S1–S4 run and check themselves: `renode/run_scenarios.sh` plays each scenario, then checks every node's log for the expected events in order (49 checks). Last run: 49 passed, 0 failed. Its checks were also run against the wrong logs to show they fail there.

| Scenario | Script | Result |
|---|---|---|
| **S1**: normal, 4 nodes, 10 s | `s1_normal.resc` | Every node OPERATIONAL from 15 ms, every peer ONLINE, `synced=20`; no LATE, OFFLINE or other fault line; every parameter copy equals its owner at every report; every receiver accepted every frame sent; no frame rejected, `log dropped=0` |
| **S2**: MCU3 halted at 5 s, reset at 10 s, run to 15 s (assignment scenarios 2–7) | `s2_slave_failure.resc` | MCU1, MCU2, MCU4 alike: P11–P15 STALE at 5025; n3 LATE at 5050, OFFLINE at 5250 (its last heartbeat was at 4900) with OWNER_OFFLINE; the other three stay OPERATIONAL and keep exchanging. At 10000 MCU3's boot heartbeat: RESTART (uptime went down; comm/ had forgotten MCU3, its `r` stays 0) and RECOVERING; flag cleared at 10005; P11–P15 FRESH at 10025 (settle rule, `held n3=1`); ONLINE recovered at 10100. MCU3 after its reset: RESYNCING at 0 ms, OPERATIONAL at 5 ms. End: every node `synced=20`, peers show `r=1 rec=1` for n3 |
| **S4**: MCU1 (master) halted at 5 s, reset at 10 s, run to 15 s (§7.5) | `s4_master_failure.resc` | MCU2, MCU3, MCU4 alike: P1–P5 STALE at 5025; n1 LATE 5050, OFFLINE 5250 with OWNER_OFFLINE 1–5; they stay OPERATIONAL and keep exchanging (`synced=15 offline=5` at 7005); no failover. At 10000: RESTART (uptime down) and RECOVERING; flag cleared 10005; P1–P5 FRESH 10025; ONLINE recovered 10100; end `synced=20`, `r=1 rec=1` for n1. MCU1 after its reset: RESYNCING at 0, OPERATIONAL at 10 ms. The UART link going silent (§7.5) is checked once the link exists |
| **S3**: MCU3 off the bus 5–15 s, run to 20 s | `s3_disconnect.resc` | MCU1, MCU2, MCU4: n3 LATE 5050, OFFLINE 5250 with the flag, RECOVERING 15000, flag cleared 15005, P11–P15 FRESH 15015 (`held n3=3`), ONLINE recovered 15100. MCU3: all three peers OFFLINE at 5250 and **self RESYNCING** until 15005, then OPERATIONAL; peers ONLINE recovered at 15100. **The first heartbeat rule at work:** MCU3 queued heartbeat 51 at 5000 (MCU1 had 50 at 5005); it stayed in the Tx FIFO and was the first heartbeat MCU1 received after OFFLINE, 10 s old and still saying OPERATIONAL. It only made n3 RECOVERING; heartbeat 52 (15000) said RESYNCING; 53 (15100) said OPERATIONAL → ONLINE. Values: 879 copies match the owner at reports outside 5–15 s, 0 problems; all 301 problems lie inside the outage |
| MCU3 off the bus 1.5–2.5 s, reset 3 ms after reconnect: an old OPERATIONAL heartbeat, then a BOOT one | `stage6_old_hb_then_boot.resc` | MCU1: OFFLINE 1750; old heartbeat 16 (sent at 1500, OPERATIONAL) → RECOVERING at 2500; boot heartbeat (sequence back after 16, BOOT) → comm/'s restart flag → RESTART at 2502, stays RECOVERING (no second line); ONLINE recovered at 2603; `r=1 rec=1`, `synced=20`. A reset clears the Tx FIFO, so BOOT can only follow the old frames, never precede them |
| Heartbeats fail the CRC from 500 ms, data fine (mutation `hbcrc`) | `stage4_mutate.sh hbcrc` | MCU1, MCU3, MCU4: `rx rejected crc id=50 n=1` at 500, n2 LATE 550, **NO_HEARTBEAT** 750, never OFFLINE; `synced=20`, `offline=0`; `rx rejected crc id=50 n=5` at 1005 (one line a second) |
| Data fails the CRC, heartbeats fine (mutation `crc`) | `stage4_mutate.sh crc` | MCU1, MCU3, MCU4 alike: n2 ONLINE at 0 (heartbeats pass); `rx rejected crc id=34 n=1` at 10; RESYNCING while waiting for node 2's data; at 350 **n2 DATA_INVALID** and **self OPERATIONAL** in the same run; `synced=15`, `offline=0`, n2 never LATE or OFFLINE; `rx rejected crc id=34 n=99` at 1005. MCU2 itself OPERATIONAL, `synced=20`. (Before DATA_INVALID: MCU1, MCU3, MCU4 stayed RESYNCING for the whole run.) |
| MCU3 halted 1.5–2.0 s | `stage4_halt.resc` | n3 LATE 1550, OFFLINE 1750, RECOVERING 2000 (its first heartbeat after resuming, which said OPERATIONAL), ONLINE recovered 2100 (1999 and 2099 in an earlier run: ±1 ms between builds). No restart counted: its uptime kept rising |
| MCU3 off the bus 1.5–2.5 s | `stage4_reconnect.resc` | OFFLINE 1750, RECOVERING 2500, ONLINE recovered 2600, `held n3=3` |
| MCU3 reset at 1.5 s | `stage4_reset.resc` | RESTART and RECOVERING at 1499 (comm/'s flag, `r=1`), ONLINE recovered 1600; never LATE (gap 19 ms) |
| Two MCUs as node 2 | `stage4_dup_node.resc` | n3 (absent) OFFLINE at 350 ms with the flag; `rx rejected dup` lines about once a second; self OPERATIONAL |
| MCU1 and MCU2 only | `stage4_two_nodes.resc` | n3 and n4 OFFLINE at 350 ms, `offline=10`, `synced=10`; self OPERATIONAL |
| 4 nodes, 3.1 s | `stage4_four_nodes.resc` | All ONLINE at 0 ms, self RESYNCING at 0, OPERATIONAL at 15 ms; no other diag line; 180/180 values, 48/48 frame counts |
| Mutations `values`, `fakeboot`; loopback | | Unchanged; all OPERATIONAL; `LOOPBACK PASS` |
| PC tests | `run_host_tests.sh` | `DIAG PASS`: boot, never heard, LATE/OFFLINE boundaries (149/150, 349/350), first heartbeat after OFFLINE, restart by flag and by uptime, uptime wrap (steady, during an outage, slack boundary 1000/1001 ms), NO_HEARTBEAT both ways, own state alone and back, ms wrap, report line. DATA_INVALID: entered at 350 ms and not before, not OFFLINE, not waited for, not ended by heartbeats, ended by a block, then LATE and OFFLINE when heartbeats stop; a node whose only peer is DATA_INVALID stays RESYNCING. Seven copies with one rule broken each fail: first-heartbeat rule 6, NO_HEARTBEAT 9, 150 ms bound 5, uptime check 2, DATA_INVALID never entered 9, DATA_INVALID waited for 2, heartbeat ending DATA_INVALID 2. `PARAMS PASS` with the owner-offline group |

## Changed or decided beyond the Design Document

| Item | Design Document | Now |
|---|---|---|
| Fault reactions | Not specified | Detect and report only |
| Own state | §7.4: OPERATIONAL when every other node is decided | BOOT until a heartbeat from any node; RESYNCING until every connected peer has sent a block; back to RESYNCING when no peer is left |
| Peer whose heartbeats stop but whose data is proper | OFFLINE | NO_HEARTBEAT |
| OFFLINE peer's parameters | Not specified | Kept (STALE), plus an owner-offline flag |
| RECOVERING → ONLINE | "Node reports OPERATIONAL with all reachable parameters synced" | A heartbeat reporting OPERATIONAL, never the first one after OFFLINE |
| UNKNOWN | Leaves only on the first heartbeat | Also → OFFLINE 350 ms after boot (§7.4 already counts such a node as OFFLINE) |
| Restart detection | comm/ only | comm/ while tracking, plus diag/ by uptime after OFFLINE |
| Rejected frames | Counted | Counted and logged, rate limited |
| Peer whose heartbeats arrive but whose blocks all fail | Not specified | DATA_INVALID after 350 ms; not waited for |

## Future scope

- **Babbling-node event** (Design Document §15, item 5): a node that floods the bus is contained by its own RAM queue (`ovw` in the driver line), but diag/ raises no event for it.
- **Master failover** (Design Document §12): when MCU1 fails, the slaves mark it OFFLINE and carry on (scenario S4), but no node takes over the UART monitoring link.
- **Bus-off detection and recovery** (Design Document §12).
