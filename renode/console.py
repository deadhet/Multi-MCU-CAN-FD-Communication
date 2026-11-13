"""One MCU's terminal for the live network you drive by hand (renode/network.sh):
shows this MCU's own log of changes as they happen, and takes commands for this MCU.

    renode/mcu.sh 3          (after renode/network.sh, in its own terminal)

Why one Renode for all four: one process gives one CAN hub and one simulated clock, so the four
nodes stay in step, as in every scenario. (Separate Renode processes could only share a bus
through Linux's vcan, with sudo, and each would keep its own clock: a node simulated faster than
another would see false LATE and OFFLINE.)

How a command reaches Renode, with no network port: renode/network.sh feeds Renode's command
monitor from a named pipe, logs/monitor.fifo (readable and writable by this user only), and
writes everything Renode prints to logs/renode.out. A command here selects this MCU
(mach set "mcuN"), writes the lines into the pipe, and reads the answer from renode.out: the
text after each command's echo, up to Renode's next prompt. A lock file keeps two terminals from
talking to Renode at the same moment. (Renode's own TCP monitor, renode -P, would listen on every
network interface, and that monitor can run Python: anyone on the same network could run code
on this PC.) Python standard library only.
"""

from __future__ import annotations

import fcntl
import os
import re
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIFO = REPO / "logs" / "monitor.fifo"   # Renode reads its commands from here (renode/network.sh)
OUT = REPO / "logs" / "renode.out"      # and everything it prints goes here
LOCK = REPO / "logs" / ".monitor.lock"

HELP = """\
  start            run the network (all four MCUs)
  pause            pause the network
  halt [s]         stop this MCU's CPU, as if it hung. With s: reset it after s seconds
                   of simulated time (scenario S2; S4 when this is MCU1)
  resume           let the halted CPU continue, without a reset
  reset            reset this MCU: it boots again
  unplug [s]       pull this MCU's CAN cable. With s: plug it back after s seconds (S3)
  plug             put the CAN cable back
  status           this MCU's last 1 s report
  time             simulated time since the network started
  wait <s>         wait s seconds of simulated time (for step files; Ctrl-C cancels)
  run <file>       run the commands in a step file, one per line; # starts a comment.
                   A bare name is looked up in renode/steps/ (run loose_cable)
  raw <command>    any Renode monitor command, run on this MCU
  help             this list
  quit             close this terminal (the network keeps running). Ctrl-C clears the line"""

STEPS = REPO / "renode" / "steps"

# Lines worth showing live: changes, not the 1 s report.
CHANGE = re.compile(r"^MCU\d (boot|t=\d+ (diag|db) |.*FAILED)")
RED, YELLOW, BLUE, BOLD, RESET = "\033[31m", "\033[33m", "\033[36m", "\033[1m", "\033[0m"
WORDS = [  # colour of each state word, as on the dashboard
    (re.compile(r"\b(OFFLINE|INVALID|DATA_INVALID|OWNER_OFFLINE|RESTART|FAILED)\b"), RED),
    (re.compile(r"\b(LATE|STALE|NO_HEARTBEAT)\b"), YELLOW),
    (re.compile(r"\b(RECOVERING|RESYNCING|BOOT|boot|UNKNOWN)\b"), BLUE),
]


# -- Renode's monitor -------------------------------------------------------------------------
class MonitorError(Exception):
    pass


@contextmanager
def monitor_lock():
    LOCK.parent.mkdir(exist_ok=True)
    with open(LOCK, "w") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        yield


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = re.compile(r"\((?:monitor|mcu\d)\) ")
LOG_ENTRY = re.compile(r"\[\d\d:\d\d:\d\d\.\d+\] \[\w+\][^\n]*\n?")  # Renode's own log lines


def answers(text: str, commands: list[str]) -> list[str] | None:
    """Each command's output in Renode's printout: from its echo to the next prompt, without
    Renode's log lines in between. None while the last command has not finished."""
    text = LOG_ENTRY.sub("", ANSI.sub("", text).replace("\r", ""))
    out, pos = [], 0
    for cmd in commands:
        i = text.find(cmd, pos)
        if i < 0:
            return None
        m = PROMPT.search(text, i + len(cmd))
        if not m:
            return None
        out.append(text[i + len(cmd):m.start()].strip())
        pos = m.end()
    return out


def monitor(node: int | None, *commands: str, timeout: float = 10.0) -> str:
    """Runs commands in Renode's monitor, on MCU node if given. Returns their output."""
    cmds = ([f'mach set "mcu{node}"'] if node else []) + list(commands)
    with monitor_lock():
        try:
            # Non-blocking: fails at once (ENXIO) if nobody reads the pipe, i.e. no network.
            fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as err:
            raise MonitorError("the network is not running: start renode/network.sh first") from err
        start = OUT.stat().st_size if OUT.exists() else 0
        try:
            os.write(fd, ("\n".join(cmds) + "\n").encode())
        finally:
            os.close(fd)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with open(OUT, "rb") as f:
                f.seek(start)
                got = answers(f.read().decode(errors="replace"), cmds)
            if got is not None:
                return "\n".join(a for a in got if a)
            time.sleep(0.05)
    raise MonitorError("Renode did not answer in time")


def sim_time() -> float:
    text = monitor(None, "emulation GetTimeSourceInfo")
    m = re.search(r"Elapsed Virtual Time: (\d+):(\d+):([\d.]+)", text)
    if not m:
        raise MonitorError(f"no simulated time in: {text!r}")
    h, mi, s = m.groups()
    return int(h) * 3600 + int(mi) * 60 + float(s)


# -- this MCU's log ---------------------------------------------------------------------------
class Console:
    def __init__(self, node: int) -> None:
        self.node = node
        self.log = REPO / "logs" / f"mcu{node}.log"
        self.prompt = f"{BOLD}mcu{node}>{RESET} "
        # For input(): readline must know which characters take no room on the screen.
        self.prompt_rl = f"\001{BOLD}\002mcu{node}>\001{RESET}\002 "
        self.tty = sys.stdin.isatty()
        self.out_lock = threading.Lock()
        self.pending: list[threading.Thread] = []  # timed actions still to happen
        self.depth = 0                              # step files running inside each other
        self.cancelled = False                      # the last wait was cancelled (Ctrl-C)

    def show(self, text: str) -> None:
        """Prints above the prompt without breaking the line being typed."""
        with self.out_lock:
            if self.tty:
                import readline
                typed = readline.get_line_buffer()
                sys.stdout.write(f"\r\033[K{text}\n{self.prompt}{typed}")
            else:
                sys.stdout.write(text + "\n")
            sys.stdout.flush()

    @staticmethod
    def colour(line: str) -> str:
        for pattern, code in WORDS:
            line = pattern.sub(lambda m, c=code: f"{c}{m.group(0)}{RESET}", line)
        return line

    def follow(self) -> None:
        """Like tail -F on this MCU's log, showing only the change lines. Renode starts a new
        file on each network start, so a file that shrank or was replaced is read again from 0."""
        f, inode, pos = None, None, 0
        while True:
            try:
                st = self.log.stat()
                if f is None or st.st_ino != inode or st.st_size < pos:
                    if f:
                        f.close()
                    f, inode, pos = open(self.log, errors="replace"), st.st_ino, 0
                while line := f.readline():  # readline, not "for line in f": tell() needs it
                    if not line.endswith("\n"):
                        f.seek(pos)  # Renode is still writing this line: read it next time
                        break
                    pos = f.tell()
                    if CHANGE.match(line):
                        self.show(self.colour(line.rstrip("\n")))
            except FileNotFoundError:
                pass
            time.sleep(0.2)

    def status(self) -> str:
        try:
            lines = self.log.read_text(errors="replace").splitlines()
        except FileNotFoundError:
            return "no log yet: the network has not started"
        report = [i for i, line in enumerate(lines) if re.match(rf"^MCU{self.node} t=\d+ tx ", line)]
        if not report:
            return "no 1 s report yet"
        block = [line for line in lines[report[-1]:] if not CHANGE.match(line)]
        return "\n".join(block)

    # -- commands -----------------------------------------------------------------------------
    def later(self, seconds: float, what: str, *commands: str) -> None:
        def run() -> None:
            try:
                t0 = sim_time()
                while sim_time() < t0 + seconds:
                    time.sleep(0.1)
                result = monitor(self.node, *commands)
                self.show(f"[mcu{self.node}: {what} after {seconds:g} s of simulated time]"
                          + (f"\n{result}" if result else ""))
            except MonitorError as err:
                self.show(f"[mcu{self.node}: {what} not done: {err}]")
        t = threading.Thread(target=run, daemon=True)
        t.start()
        self.pending = [p for p in self.pending if p.is_alive()] + [t]

    def wait(self, seconds: float) -> bool:
        """Waits seconds of simulated time. False if cancelled with Ctrl-C."""
        try:
            t0 = sim_time()
            last, still = t0, time.monotonic()
            while (now := sim_time()) < t0 + seconds:
                if now != last:
                    last, still = now, time.monotonic()
                elif time.monotonic() - still > 3.0:
                    self.show("[waiting: simulated time is not moving; is the network paused? Ctrl-C cancels]")
                    still = time.monotonic()
                time.sleep(0.1)
            return True
        except KeyboardInterrupt:
            self.show("[wait cancelled]")
            return False

    def run_file(self, name: str) -> bool:
        """The commands in a step file, one per line. False if the file asked to quit."""
        candidates = [Path(name), REPO / name, STEPS / name, STEPS / f"{name}.txt"]
        path = next((p for p in candidates if p.is_file()), None)
        if path is None:
            self.show(f"no step file {name!r} (looked in the current folder, the repo and renode/steps/)")
            return True
        if self.depth >= 3:
            self.show(f"[{path.name}: step files nested too deep; not run]")
            return True
        self.show(f"[running {path}]")
        self.depth += 1
        try:
            for raw_line in path.read_text().splitlines():
                line = raw_line.split("#", 1)[0].strip()
                if not line:
                    continue
                self.show(f"{self.prompt}{line}")
                if line.split()[0] in ("quit", "exit"):
                    break  # ends the step file; a step file never closes the terminal
                self.cancelled = False
                self.run(line)
                if self.cancelled:  # Ctrl-C during a wait
                    self.show(f"[{path.name} stopped]")
                    return True
            self.show(f"[{path.name} done]")
            return True
        except KeyboardInterrupt:
            self.show(f"[{path.name} stopped]")
            return True
        finally:
            self.depth -= 1

    def run(self, line: str) -> bool:
        """One command. False to quit."""
        line = line.split("#", 1)[0]
        word, _, rest = line.strip().partition(" ")
        rest = rest.strip()
        n = self.node
        if not word:
            return True
        if word in ("quit", "exit"):
            return False
        if word == "help":
            self.show(HELP)
            return True
        if word == "status":
            self.show(self.status())
            return True
        if word == "run":
            if not rest:
                self.show("run <file>: which step file? (ls renode/steps)")
                return True
            return self.run_file(rest)
        try:
            secs = float(rest) if rest and word in ("halt", "unplug", "wait") else None
        except ValueError:
            self.show(f"not a number of seconds: {rest!r}")
            return True
        if word == "wait":
            if secs is None or secs <= 0:
                self.show("wait <s>: how many seconds of simulated time?")
                return True
            try:
                self.cancelled = not self.wait(secs)
            except MonitorError as err:
                self.show(f"[{err}]")
                self.cancelled = True
            return True
        try:
            if word == "start":
                out = monitor(None, "start")
            elif word == "pause":
                out = monitor(None, "pause")
            elif word == "time":
                out = f"simulated time: {sim_time():.3f} s"
            elif word == "halt":
                out = monitor(n, "sysbus.cpu IsHalted true")
                self.show(f"[mcu{n}: CPU halted]")
                if secs:
                    self.later(secs, "reset", "machine Reset", "sysbus.cpu IsHalted false")
            elif word == "resume":
                out = monitor(n, "sysbus.cpu IsHalted false")
            elif word == "reset":
                out = monitor(n, "machine Reset", "sysbus.cpu IsHalted false")
                self.show(f"[mcu{n}: reset]")
            elif word == "unplug":
                out = monitor(n, "connector Disconnect sysbus.fdcan1 canHub")
                self.show(f"[mcu{n}: CAN cable out]")
                if secs:
                    self.later(secs, "CAN cable back in", "connector Connect sysbus.fdcan1 canHub")
            elif word == "plug":
                out = monitor(n, "connector Connect sysbus.fdcan1 canHub")
                self.show(f"[mcu{n}: CAN cable in]")
            elif word == "raw" and rest:
                out = monitor(n, rest)
            else:
                self.show(f"unknown command {word!r}; type help")
                return True
        except MonitorError as err:
            self.show(f"[{err}]")
            return True
        if out:
            self.show(out)
        return True


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in ("1", "2", "3", "4"):
        print("usage: renode/mcu.sh 1|2|3|4", file=sys.stderr)
        return 2
    con = Console(int(sys.argv[1]))
    role = "master" if con.node == 1 else "slave"
    print(f"{BOLD}MCU{con.node} ({role}){RESET}: its own log of changes appears here as it happens.\n"
          f"Commands act on MCU{con.node}; 'start' and 'pause' on the whole network. Type help.")
    threading.Thread(target=con.follow, daemon=True).start()
    if con.tty:
        import readline  # noqa: F401  (line editing and history for input())
    while True:
        try:
            line = input(con.prompt_rl) if con.tty else sys.stdin.readline()
        except KeyboardInterrupt:  # Ctrl-C clears the line, as in a shell
            print()
            continue
        except EOFError:  # Ctrl-D
            print()
            return 0
        if not con.tty and not line:
            # Commands piped in (renode/mcu.sh 3 < steps.txt) have ended: let the timed actions
            # still pending happen, then give the log a moment to show them.
            for t in con.pending:
                t.join()
            time.sleep(1.0)
            return 0
        try:
            if not con.run(line):
                return 0
        except KeyboardInterrupt:
            con.show("[cancelled]")


if __name__ == "__main__":
    sys.exit(main())
