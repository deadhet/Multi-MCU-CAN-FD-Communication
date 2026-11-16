# Running the system in Renode

Renode 1.17.0. Build first: `cmake --build build`. All commands from the repo root.

## What is in `renode/`

| File | Use |
|---|---|
| `system.resc`, `node.repl` | The four nodes on one CAN hub; included by everything else |
| `network.sh`, `mcu.sh`, `console.py` | **Drive the network by hand**: one terminal per MCU (below) |
| `prompt.sh` | **One terminal**: Renode's own prompt and the dashboard (below) |
| `demo.sh`, `demo.resc` | A scripted run with the dashboard: `demo.resc`, or one of `s1`–`s4` |
| `s1_normal.resc` … `s4_master_failure.resc` | The Design Document §9 scenarios |
| `run_scenarios.sh` | Runs S1–S4 and checks every node's log (49 checks) |
| `check_uart_link.sh` | Runs one scenario and checks MCU1's UART link end to end |
| `tests/` | Break-test benches (see `fault_testing.md`): loopback self test, 2- and 4-node runs, halt, reset, reconnect, duplicate node, frame mutations, receive overload, counter overflow, cross-checks |

Every run writes the nodes' debug output to `logs/mcu1.log` … `logs/mcu4.log` (ignored by git).

## Driving the network by hand (four terminals)

```
terminal 0:  renode/network.sh          # Renode with all four MCUs, paused, and the dashboard
terminal 1:  renode/mcu.sh 1
terminal 2:  renode/mcu.sh 2
terminal 3:  renode/mcu.sh 3
terminal 4:  renode/mcu.sh 4
```

Type `start` in any MCU terminal. Each MCU terminal shows that MCU's own log of changes as they happen (its view of the others: LATE, OFFLINE, RECOVERING, STALE …, coloured as on the dashboard) and takes commands for that MCU:

| Command | Does | Scenario |
|---|---|---|
| `halt` | Stops the MCU's CPU, as if it hung | |
| `halt 5` | Halts it, then resets it after 5 s of simulated time | S2 (a slave), S4 (MCU1) |
| `resume` | The halted CPU continues, without a reset | |
| `reset` | Resets the MCU: it boots again | |
| `unplug` | Pulls its CAN cable | |
| `unplug 10` | Pulls it, plugs it back after 10 s of simulated time | S3 |
| `plug` | Plugs the cable back | |
| `status` | Its last 1 s report | |
| `time` | Simulated time since start | |
| `wait <s>` | Waits s seconds of simulated time (for step files) | |
| `run <file>` | Runs a step file: one command per line, `#` comments; a bare name is looked up in `renode/steps/` (`run loose_cable`) | |
| `start`, `pause` | The whole network | |
| `raw <command>` | Any Renode monitor command, on this MCU | |
| `help`, `quit` | `quit` closes the terminal only; the network keeps running | |

Times are simulated time. Renode runs at about 0.3× real time on the development PC, so `halt 5` takes about 17 s of wall time. Timed actions land within about 0.1 s of simulated time.

An MCU terminal is not a shell: it understands only the commands above. Sequences go in **step files** (`renode/steps/`: `loose_cable.txt`, `hang_then_crash.txt`), run with `run <name>`, or piped in from a shell: `renode/mcu.sh 3 < my_steps.txt`. A heartbeat due while a cable is out waits in the transmit buffer, so the others see a gap of the drop plus up to 100 ms: a 0.15–0.25 s drop is always LATE and never OFFLINE, from 0.35 s it is always OFFLINE.

**Never `unplug` an MCU whose cable is already out** (including while an `unplug 10` is still out). A second `connector Disconnect` crashes Renode 1.17 (`KeyNotFoundException: The given key 'Antmicro.Renode.Peripherals.CAN.MCAN' was not present in the dictionary`), and the whole network stops. `plug` when already in only gets an error message.

### Why one Renode, not four

One Renode process gives one CAN hub and one simulated clock: the four nodes stay in step, as in every scenario and check. Four Renode processes could only share a bus through Linux's virtual CAN (Renode's SocketCAN bridge: needs `sudo` for `vcan0`, and CAN FD support depends on the host), and each would keep its own clock. A node simulated faster than another would then see false LATE and OFFLINE.

### How the terminals reach Renode

`network.sh` feeds Renode's command monitor from a named pipe, `logs/monitor.fifo` (mode 600: this user only), and writes everything Renode prints to `logs/renode.out`. An MCU terminal writes its commands into the pipe (`mach set "mcuN"` first) and reads the answer from `renode.out`, under a lock file so two terminals never interleave. Renode's own TCP monitor (`renode -P`) was not used: it listens on every network interface and has no option to limit that, and the monitor can run Python, so anyone on the same network could run code on the PC.

Renode's UART terminal for the dashboard (`system.resc`, TCP port 3456) also listens on every interface: someone on the same network could read MCU1's monitoring stream while a simulation runs. It carries no commands. On an untrusted network, enable the firewall (`sudo ufw enable`).

## One terminal: Renode's own prompt

Without `network.sh` and the MCU terminals, Renode's own commands can be typed at its prompt:

```
renode/prompt.sh                 # Renode at its prompt, four MCUs paused, and the dashboard
renode/prompt.sh --no-dashboard  # Renode only
```

The dashboard runs in the background (its messages in `logs/dashboard.out`); `quit` ends Renode and stops it. Renode starts with `logLevel 3`, which hides the start-up warnings (registers Renode does not model). The prompt starts at `(mcu1)`: `system.resc` selects MCU1 last. Then `start`.

By hand, the script does: `renode --console -e "logLevel 3; include @renode/system.resc"`, with the dashboard started from `dashboard/` (`.venv/bin/python -m noccarc_dashboard.server --lost-ms 1500`).

| Command | Does |
|---|---|
| `start`, `pause` | The whole network |
| `mach set "mcu3"`, `mach clear`, `mach` | Select MCU3 (prompt `(mcu3)`), select none, list the MCUs. Every command below acts on the selected MCU |
| `sysbus.cpu IsHalted true` / `false` | Halt its CPU / let it continue |
| `machine Reset`, then `sysbus.cpu IsHalted false` | Reset it (the reset macro in `system.resc` loads the firmware again) |
| `connector Disconnect sysbus.fdcan1 canHub` | Pull its CAN cable: only when it is in (see above) |
| `connector Connect sysbus.fdcan1 canHub` | Plug it back |
| `emulation RunFor "5.0"` | From paused: run exactly 5 s of simulated time, then pause. Refused while running |
| `emulation GetTimeSourceInfo` | Simulated time (`Elapsed Virtual Time`) |
| `showAnalyzer sysbus.usart1` | Its debug output: a window, or in this terminal with `--disable-gui` (then `logLevel 1 sysbus.usart1` to show it past `logLevel 3`). `logs/mcuN.log` is still written |
| `include @file.resc` | Run the commands in a file |
| `quit` | End Renode |

Scenario S2 with exact timing, typed or saved in a file and run with `include` (S4: `mcu1`; S3: `connector Disconnect …`, `RunFor "10.0"`, `connector Connect …`):

```
pause
mach set "mcu3"
sysbus.cpu IsHalted true
emulation RunFor "5.0"
machine Reset
sysbus.cpu IsHalted false
start
```

The scenario files (`s1_normal.resc` …) load the four MCUs themselves: start Renode with one (`renode --console -e "include @renode/s2_slave_failure.resc"`) rather than including it at a loaded prompt.

## Scripted runs

```
renode/demo.sh                 # dashboard + demo.resc (about 2.5 min)
renode/demo.sh s3              # dashboard + scenario S3
renode/run_scenarios.sh        # S1-S4 with all 49 log checks
renode/check_uart_link.sh s4   # S4 with the UART link checks
```
