# Architecture and code flow

How the firmware runs, end to end, from `main()` down to the drivers and the hardware. Everything
here is traced from the source (`file:line` references point at the real lines).

The same binary serves every node, selected by `NODE_ID`: node 1 is the master (`app_master.c`),
nodes 2–4 are slaves (`app_slave.c`). `app_slave.c` is `app_master.c` with the UART-monitor half
removed.

## The two `main()`s — same skeleton, the master does more

Both apps are a bare-metal **super-loop**: no RTOS, one foreground thread (the loop) plus a few
ISRs in the background.

```
          BOOT (runs once)                         app_master.c   app_slave.c
  ─────────────────────────────────────────────    ────────────   ───────────
  board_init()   clock→systick→usart1→fdcan pins        ✓              ✓
  params_init()                                          ✓              ✓
  diag_init()                                            ✓              ✓
  uart_debug_write("MCUn boot")                          ✓              ✓
  comm_init()  → fdcan_init(NORMAL, filters)             ✓              ✓
        └─ returns false?  →  print "CAN init FAILED", spin forever (fail-fast)
  uart_link_init()   (USART3 + tx_slots double-buffers)  ✓              ✗  (master only)
  monitor_init()     (nanopb/cobs wiring)                ✓              ✗  (master only)
  sched_init(tasks)   7 tasks (master) / 5 tasks (slave) ✓              ✓
```

Then the loop runs forever. The master's loop has **two extra `look_for_events()` calls** (EVENT
detection for the dashboard); the slave's loop is otherwise identical:

```
  for (;;) {                                          app_master   app_slave
    sched_pass_mark();        // stamp cycle counter      ✓            ✓
    sched_run_due();          // run every task whose period elapsed
    look_for_events(false);   // master: emit EVENT if a count moved  ✗
    while (comm_receive(&frame)) {        // drain the CAN RX ring
        if (frame.type == PARAM) {
            params_apply_block(...);      // store into the 20-param DB
            diag_on_param_block(...);     // "data still arriving" signal
        } else {                          // HEARTBEAT
            diag_on_heartbeat(...);       // liveness + restart detection
        }
        look_for_events(false);           // master only                ✗
    }
  }
```

Two loads per pass: **run due tasks**, then **drain whatever the CAN ISR has queued**. That is the
whole foreground.

## The task table (the scheduler's work list)

`sched_run_due()` (`firmware/common/sched/scheduler.c:38`) walks this table; a task runs when
`(now − last_run) ≥ period` — unsigned, so it survives the 49.7-day `ms_counter` wrap.

| Task | Period | Master | Slave | What it calls down to |
|---|---|---|---|---|
| `param` | 10 ms | ✓ | ✓ | `param_source_task` → `params_build_block` → `comm_send_param_block` → **`fdcan_send`** |
| `fresh` | 5 ms | ✓ | ✓ | `params_update_freshness` (ages the 20 params) |
| `diag` | 5 ms | ✓ | ✓ | `diag_update` → `comm_forget_peer` / `params_set_owner_offline` |
| `hb` | 100 ms | ✓ | ✓ | `comm_send_heartbeat` → **`fdcan_send`** |
| `report` | 1000 ms | ✓ | ✓ | `comm/params/diag/sched_debug_report` → **`uart_debug_write`** (USART1) |
| `snap` | 100 ms | ✓ | — | `monitor_send_snapshot` → nanopb → COBS → **`uart_link`** (USART3) |
| `cfg` | 1000 ms | ✓ | — | `monitor_send_config` → … → **`uart_link`** (USART3) |

## Layer stack — foreground vs. the ISRs

```
┌──────────────────────────────────────────────────────────────────────────┐
│  APP        app_master.c / app_slave.c   — super-loop + task table         │
├──────────────────────────────────────────────────────────────────────────┤
│  SCHED      sched/scheduler.c            — cooperative "run if due"         │
├──────────────────────────────────────────────────────────────────────────┤
│  SERVICES   comm/   params/   diag/   monitor/(+cobs,+nanopb)              │
│             validate   20-param DB  health   build UART frames             │
├──────────────────────────────────────────────────────────────────────────┤
│  DRIVERS    fdcan.c  frame_queue.c  uart.c  uart_link.c/tx_slots.c         │
│             systick.c  cycles.c  clock.c  board.c                          │
├──────────────────────────────────────────────────────────────────────────┤
│  HARDWARE   FDCAN1   USART1   USART3   SysTick   RCC/PLL                    │
└──────────────────────────────────────────────────────────────────────────┘

  FOREGROUND  : the super-loop (tasks + RX drain)  ── cooperative, never blocks
  BACKGROUND  : 4 ISRs, all driver-level:
     SysTick_Handler        systick.c:10     ms_counter++        (1 ms)
     FDCAN1_IT0_IRQHandler  fdcan.c:300      HW FIFO → SW ring   (CAN RX)
     USART1_IRQHandler      uart.c:78        byte pump, debug TX
     USART3_IRQHandler      uart_link.c:74   byte pump, monitor TX (master only)
```

## End-to-end data flows

### 1. CAN transmit (param block / heartbeat)

```
param_task / heartbeat_task            [FOREGROUND, scheduler]
   → comm_send_param_block/heartbeat   comm.c:195/206  (adds CRC16, version, seq++, src)
      → fdcan_send(id, buf, len)        fdcan.c:202
         → write TX-FIFO element in message RAM
         → FDCAN1->TXBAR = 1<<put       fdcan.c:235   (hardware starts sending)
                                        ─────────────► CAN FD bus
```

### 2. CAN receive (ISR → ring → drain → validate → services)

```
CAN FD bus ─────►  FDCAN1 hardware RX FIFO 0 (16 deep)
   │  RF0N interrupt
   ▼  [BACKGROUND]
FDCAN1_IT0_IRQHandler            fdcan.c:300
   while FIFO not empty:
      store_rx_element(index)    fdcan.c:277
         ├─ queue_for(id)        route by CAN-ID to the right software queue
         ├─ stamp slot->rx_time_ms = systick_now_ms()   ◄── arrival time fixed HERE
         └─ frame_queue_commit() frame_queue.c   (lock-free ring; full ⇒ overwrite oldest)
   ────────────────────────────────────────────────────────────────────
   [FOREGROUND, next loop pass]
while comm_receive(&frame)        comm.c:352
   → fdcan_receive(q, &out)       fdcan.c:239  → frame_queue_pop()
   → comm validates: CRC · version · source · length · FD/BRS · sequence(lost/dup/old)
      good → returns frame to main() → params_apply_block / diag_on_heartbeat
      bad  → counted (crc/ver/src/len/old/dup…), dropped, never reaches the DB
```

The key seam: the **ISR only copies bytes and timestamps**; all *validation* happens in the
foreground in `comm_receive`, so the interrupt stays short and arrival time is exact even under
load.

### 3. UART debug log (all four nodes → `logs/mcuN.log`)

```
report_task (+ any uart_debug_write)      [FOREGROUND]
   → uart_debug_write(text)        uart.c:31   push into 2 KB SW ring (tx_head)
                                               overflow ⇒ whole write dropped + counted
   → enable USART1 TXEIE           uart.c:54
        │  TXE interrupt
        ▼  [BACKGROUND]
   USART1_IRQHandler               uart.c:78   pop one byte → USART1->TDR; empty ⇒ TXEIE off
                                   ──────────► USART1 pin → Renode → logs/mcuN.log
```

### 4. UART monitor link (MCU1 only → dashboard)

```
snapshot_task / config_task / look_for_events   [FOREGROUND, master]
   → monitor_send_snapshot/…       monitor.c
      → pb_encode(...)             nanopb: struct → Protobuf bytes
      → cobs_encode(...)           cobs.c: frame with 0x00 delimiter
      → tx_slots / uart_link       uart_link.c  double-buffer (snap/cfg) + event queue
          replaced-while-busy ⇒ newest wins, old one counted (never blocks the loop)
      → enable USART3 TXEIE        uart_link.c:49
           │  TXE interrupt
           ▼  [BACKGROUND]
      USART3_IRQHandler            uart_link.c:74  pop byte → USART3->TDR
                                   ──────────► USART3 → TCP 3456 → dashboard (live decode)
```

### 5. Timebase (feeds every "is it due / is it late / how old" decision)

```
SysTick hardware (1 ms) ──► SysTick_Handler  systick.c:10  → ms_counter++
                                   │
         systick_now_ms() reads it everywhere ▼
   scheduler "due?"   diag ages/timeouts   param freshness   heartbeat uptime   rx_time_ms
   — all computed as (now − earlier) unsigned, so they stay correct across the 2³² wrap.
```

## Summary

`main()` initialises the board and the four services, then spins a cooperative super-loop that
does exactly two things each pass: **run any scheduler task whose period has elapsed**, and
**drain the CAN RX ring that the FDCAN ISR has been filling**. Tasks push data *outward*
(`comm_send_* → fdcan_send`, `uart_debug_write`, `monitor_send_* → uart_link`); the RX drain pulls
data *inward* (`comm_receive → validate → params/diag`). Four ISRs sit underneath — SysTick for the
clock, FDCAN for CAN RX, and the two USART byte-pumps — so the foreground never blocks on I/O. The
master additionally runs the snapshot/config/event path out USART3 to the dashboard; a slave is the
same machine with that half omitted.
