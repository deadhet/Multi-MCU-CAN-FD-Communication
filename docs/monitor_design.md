# monitor/ module and the UART link transmit side

Covers `firmware/common/monitor/` and the transmit drivers `firmware/common/drivers/uart_link.c` and `tx_slots.c`. MCU1 only (`monitor.c` refuses to build for another node). Section numbers (§) refer to the Design Document.

`docs/uart_protocol.md` specifies what goes over the wire (frame, messages, fields, the receiver's checks). This file describes how MCU1 makes those frames and gets them out.

## What it does

| Job | Where | Design |
|---|---|---|
| Builds SNAPSHOT (every 100 ms), CONFIG (every 1 s) and EVENT frames | `monitor_send_snapshot()`, `monitor_send_config()`, `monitor_send_events()` | §8.3 |
| Finds changes for events: compares a view with the one seen last time | `monitor_send_events()`, `seen` | §8.3 |
| Packs the Protobuf payload (nanopb), adds header and CRC-16, COBS-encodes | `build_frame()`, `cobs_encode()` | §8.2 |
| Gives every frame a sequence number, also one that is never sent | `uart_seq` | §8.4 |
| Network health: the weaker of online/4 and synced/20 | `monitor_online_count()`, `monitor_health_percent()` | §8.3 |
| Latest-wins slots for SNAPSHOT and CONFIG, a 16-entry queue for EVENTs | `drivers/tx_slots.c` | §8.4 |
| Sends the bytes: USART3 interrupt, one byte per TXE | `drivers/uart_link.c`, `USART3_IRQHandler()` | §8.4 |

It never waits for the link, and it calls no other service.

## The view: copies, not calls

`monitor/` may not call `params/` or `diag/` (§4.4). The app copies what it needs into one `monitor_view_t` and passes a pointer to it:

| Field | Filled by `collect_view()` (`app_master.c`) from |
|---|---|
| `now_ms` | `systick_now_ms()` |
| `own_state` | `diag_own_state()` |
| `synced_count` | `params_synced_count()` |
| `own_error_summary` | the app's `error_summary()`, as in MCU1's heartbeat |
| `params[20]` | `params_get()` for P1–P20 |
| `peers[4]` | `diag_get_peer()` for each node (MCU1's own entry is unused) |

- The view is over 1 KB, so it is a static variable in `app_master.c`, not on the stack.
- One view serves the event look, the snapshot and the 1 s report's health line. They all run in the main loop, never at the same time.
- `monitor/` includes `comm.h`, `diag.h` and `params.h` only for these types.

## Event detection

**When MCU1 looks.** `diag/` and `params/` each keep a change count (`diag_change_count()`, `params_change_count()`): it goes up whenever a state the link reports changes. `look_for_events(every_time)` in `app_master.c`:

1. If neither count moved since the last look and `every_time` is false, it returns at once.
2. Otherwise it remembers the counts, calls `collect_view()`, then `monitor_send_events(&view)`.
3. If no count had moved (only possible with `every_time`) but events were still made, it adds them to `events_unflagged`. That would mean `diag/` or `params/` missed counting a change. It must stay 0.

The main loop calls `look_for_events(false)` after the task pass and after each CAN frame drained. `snapshot_task()` calls `look_for_events(true)` first and then sends the snapshot from the same view, so the events always explain the snapshot that follows them.

**What counts as a change.** `monitor_send_events()` compares the view with `seen`, its copy of what the previous call saw. `monitor_init()` sets `seen` to the boot state: every parameter NEVER_RECEIVED and STALE, no owner offline, every peer UNKNOWN with 0 restarts, own state BOOT. So the first look reports everything that changed since boot.

**One event per kind of change**, with a bit mask of every node or parameter that made it (bit n−1 = MCUn or Pn), in this order:

| Order | Event | Compared field |
|---|---|---|
| 1 | Peers that restarted | `peers[].restarts` |
| 2 | Peers whose link changed, one event per new link state (UNKNOWN … SELF) | `peers[].link` |
| 3 | MCU1's own state | `own_state` |
| 4 | Parameters: OWNER_WENT_OFFLINE, OWNER_BACK, BECAME_VALID, BECAME_INVALID, BECAME_FRESH, BECAME_STALE | `owner_offline`, `validity`, `freshness` |

- **Nodes before parameters:** a node failing is the cause of its parameters going stale.
- **Validity:** any change away from VALID is reported as BECAME_INVALID. NEVER_RECEIVED cannot come back, so the host needs no third case.
- Every event carries `view->now_ms`. `seen` is updated as each change is found, so nothing is reported twice.
- The return value is the number of events made, queued or dropped. The app uses it only for `events_unflagged`.

## Building a frame

All three kinds go through the same steps:

| Step | SNAPSHOT | CONFIG | EVENT |
|---|---|---|---|
| 1. Get a buffer | `begin_frame()` → `uart_link_begin()`. A waiting snapshot is taken back here, before the struct is filled, so this snapshot already counts it in `frames_skipped` | same, CONFIG slot | `uart_link_queue_begin()`: the entry at the back of the queue, or NULL if it is full |
| 2. Fill the C struct | `snapshot` (static) | `config` (static) | `monitor_Event e` (local) |
| 3. `build_frame()` | ← same function for all three → | | |
| 4. Hand it over | `send_frame()` → `uart_link_commit()` | same | `uart_link_queue_commit()` |

`build_frame()` (`monitor.c:111`):
1. `pb_encode()` writes the Protobuf payload into the scratch buffer `frame[]`, from byte 5.
2. Byte 0: version and type. Bytes 1–4: `uart_seq + 1`, little-endian.
3. CRC-16 over bytes 0 to the end of the payload, appended.
4. `cobs_encode()` copies and encodes the frame into the buffer from step 1; one `0x00` ends it.
5. If the encode fails, or the result would not fit (cannot happen: the buffers are sized from the generator), it counts `encode_errors` and returns 0, and nothing is handed over.

`frame[]` is shared by all three kinds. Only the main loop uses it, one `build_frame()` at a time, and the interrupt never reads it.

**Sequence numbers.** `uart_seq` goes up by one for every frame handed over, and also for every event the full queue refused. A replaced snapshot or CONFIG has used its number too. Every number the host never receives is therefore either counted on MCU1 (`frames_skipped`) or lost on the way.

## Buffers and memory

Every size comes from the generator's maximum message size (`monitor.pb.h`) plus 5 bytes of header, 2 of CRC, the COBS overhead (`COBS_MAX_ENCODED`: one byte, plus one per 254) and the terminator:

| Buffer | Declared | Size |
|---|---|---|
| `frame[]`, scratch for the unencoded frame | `monitor.c:60` | 1,053 B (payload up to 1,046) |
| `snapshot_wire[2]`, the SNAPSHOT slot | `monitor.c:61` | 2 × 1,059 B |
| `config_wire[2]`, the CONFIG slot | `monitor.c:62` | 2 × 819 B (payload up to 807) |
| `event_wire[16]`, the event queue | `monitor.c:63` | 16 × 25 B (payload up to 16) |

About 5.2 KB in all. The buffers, and the `snapshot` and `config` structs, are in `.noinit` RAM (`util/noinit.h`): start-up does not clear them (each is completely written before it is read), and DMA1/DMA2 can reach that RAM, which they cannot do for DTCM.

`monitor_init()` hands the slot and queue buffers to the link once (`uart_link_attach()`, `uart_link_attach_queue()`). `monitor.c` owns the memory; `tx_slots.c` keeps only the addresses and sizes.

## Transmit slots and the byte mover (`tx_slots.c`, `uart_link.c`)

Two contexts share the buffers: the **writer** (main loop: `*_begin()`, `*_commit()`) and the **mover** (the USART3 interrupt: `tx_slots_next()`, `tx_slots_done()`). The main loop never interrupts the mover. No interrupt is ever masked.

**A slot (SNAPSHOT or CONFIG)** has two buffers and one atomic `pending` field: which buffer holds a frame handed over but not started, or none.
- `tx_slots_begin()` takes `pending` with an atomic exchange. If a frame was pending, it is taken back (counted as `replaced`) and its buffer is written again. If not, the writer gets the buffer the mover is not sending.
- `tx_slots_commit()` stores the length and an order number, then sets `pending` (release).
- So each kind has at most one frame on its way out and one waiting.

**The event queue** is a ring of 16 fixed-size entries with free-running `tail` (moved only by the writer) and `head` (moved only by the mover). `tx_slots_queue_begin()` refuses when 16 are in use (`queue_full`). It needs no exchange.

**Order.** Every hand-over, slot or queue, gets its number from one counter, `handovers`. `tx_slots_next()` looks at the pending SNAPSHOT, the pending CONFIG and the front of the queue, and takes the lowest number: frames leave in the order they were handed over, whatever their kind. The comparison is a signed difference, `(int32_t)(a − b) < 0`, so it stays right when the counter wraps.

**Taking a slot frame.** `tx_slots_next()` takes it with the same atomic exchange as `tx_slots_begin()`, so exactly one of them gets a pending frame. If the interrupt arrives in the middle of the writer's exchange (LDREX/STREX on Cortex-M), the writer's store fails and it tries again. A queue entry is not freed when it starts: `head` moves only in `tx_slots_done()`.

**`tx_slots_done()`**, after the last byte: moves `head` for an event, or marks the slot's buffer free; counts `sent` and `bytes`.

**The mover** (`USART3_IRQHandler()`): when it has no frame it asks `tx_slots_next()`; it writes one byte to `TDR`; after the last byte it calls `tx_slots_done()`. With nothing pending it clears `TXEIE` and goes quiet. Both commit functions set `TXEIE` again.

On hardware a DMA stream can replace the interrupt without changes outside `uart_link.c` (`uart_link.h`). Renode's USART has no transmit DMA request.

## Accounting

| Counter | Where | Counts |
|---|---|---|
| `snapshots`, `configs`, `events` | `monitor_stats_t` | Frames handed over |
| `events_dropped` | `monitor_stats_t` | Events refused, queue full |
| `skipped` | `monitor_stats_t` | Frames never sent: replaced, or refused. Sent in every snapshot as `frames_skipped` |
| `encode_errors` | `monitor_stats_t` | Frames not encoded (should never happen) |
| `last_snapshot_bytes`, `max_snapshot_bytes` | `monitor_stats_t` | Snapshot size on the wire, terminator included |
| `handed`, `replaced`, `queued`, `queue_full`, `queue_peak`, `sent`, `bytes` | `tx_slots_stats_t` | The link side: hand-overs, take-backs, queue use, frames and bytes finished |
| `events_unflagged` | `app_master.c` | Changes no count flagged (must stay 0) |

## Network health

- **Online:** MCU1 itself, plus every peer whose link is ONLINE or LATE. One late heartbeat is not yet a failure, so health does not dip for it. OFFLINE, RECOVERING, NO_HEARTBEAT, DATA_INVALID and never-heard peers do not count.
- **Health:** each part in whole percent, rounded down, then the weaker: `min(online × 100 / 4, synced × 100 / 20)`. 4/4 and 20/20 → 100; 3/4 and 15/20 → 75; 4/4 and 0/20 → 0.
- The snapshot and the 1 s report use the same two functions, so the dashboard and the log always agree.

## Debug output

Every second, in MCU1's report (`report_task()`):

    MCU1 health=100 online=4/4 synced=20/20
    MCU1 link sent=326 bytes=124478 snap=251 cfg=26 ev=49 ev_drop=0 ev_peak=1 ev_unflagged=0 skipped=0 enc_err=0 snap_bytes=451/464

(from a Renode run, `logs/mcu1.log`)

| Field | Meaning |
|---|---|
| `sent`, `bytes` | Frames and bytes the mover finished |
| `snap`, `cfg`, `ev` | SNAPSHOTs, CONFIGs and events handed over |
| `ev_drop` | Events refused, queue full |
| `ev_peak` | Most entries the event queue held at once, of 16 |
| `ev_unflagged` | Changes no count flagged; must be 0 |
| `skipped` | Frames never sent (replaced or refused) = `frames_skipped` |
| `enc_err` | Frames not encoded |
| `snap_bytes` | Last / largest snapshot on the wire |

## Tests

| Test | Shows |
|---|---|
| `firmware/tests/host/test_monitor.c` (`MONITOR PASS`) | `test_cobs`, `test_snapshot`, `test_config`, `test_latest_wins`, `test_health`, `test_worst_case`, `test_events`; details in `docs/uart_protocol.md` §11 |
| `firmware/tests/host/test_tx_slots.c` (`TX SLOTS PASS`) | Writer and mover played step by step: fixed cases, the event queue, then 200,000 random steps checked against the rules after every step |
| `dashboard/tests/test_framing.py` | The Python decoder reads the C test's frames and finds the same values |
| `renode/check_uart_link.sh s1`…`s4` | End to end in Renode; see `docs/uart_protocol.md` §11 |
| `renode/tests/counter_overflow.sh` | `uart_seq` starts at `0xFFFFFFE2` and wraps about 3 s into the run; the host sees 0 lost and 0 missing frames (`docs/fault_testing.md` §4) |

## Limitations

- **Renode does not pace the UART:** bytes leave at once, so a replaced snapshot or a full event queue never happens there. Only the host tests cover those paths.
- **No test drives `handovers` (the send-order counter in `tx_slots.c`) across its 32-bit wrap.** Its comparison is a signed difference, so it is wrap-safe by review. `uart_seq` is tested across its wrap (`counter_overflow.sh`).
- **Encode time** was measured only in Renode (`snap` in the scheduler line, `docs/sched_design.md`).

## Future scope

- **A DMA mover on the board**, with the same buffers (`uart_link.h`).
- **Measure `ev_peak` on the board**, on a paced link, to confirm that 16 queue entries are enough (`docs/uart_protocol.md` §13).
- **A host test across the `handovers` wrap** in `test_tx_slots.c`, by starting the counter just below 2^32.
