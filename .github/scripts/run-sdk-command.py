"""Keep SDK build progress visible without streaming the full compiler log."""

import argparse
from collections import deque
from pathlib import Path
from queue import Empty, Queue
import re
import subprocess
from threading import Thread
import time


PHASE = re.compile(r"--- Building .+ ---|Configuring: .*|Building & installing: .*|Installed: .*")
NINJA = re.compile(r"^\[(\d+/\d+)\]")
ERROR = re.compile(r"^FAILED:|\bfatal error\b|\bCMake Error\b|\berror [CL][A-Z]*\d+:|\berror:", re.I)


def read_output(stream, messages):
    try:
        for line in stream:
            messages.put(line.rstrip())
    finally:
        messages.put(None)


def run_sdk_command(log_path, command, interval):
    log_path.parent.mkdir(parents=True, exist_ok=True)
    messages = Queue(maxsize=1024)
    tail = deque(maxlen=35)
    errors = deque(maxlen=20)
    phase = "starting"
    latest = ""
    started = time.monotonic()
    next_report = started + interval

    with log_path.open("w", encoding="utf-8") as log, subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        encoding="utf-8", errors="replace",
    ) as process:
        reader = Thread(target=read_output, args=(process.stdout, messages), daemon=True)
        reader.start()

        while True:
            try:
                line = messages.get(timeout=1)
            except Empty:
                line = ""

            if line is None:
                break
            if line:
                print(line, file=log)
                tail.append(line)
                if PHASE.fullmatch(line.strip()):
                    phase = line.strip()
                    latest = ""
                    print(phase, flush=True)
                elif match := NINJA.match(line):
                    latest = match[1]
                if ERROR.search(line):
                    errors.append(line)

            now = time.monotonic()
            if now >= next_report:
                progress = f" | Ninja {latest}" if latest else ""
                print(f"[{int(now - started)}s] {phase}{progress}", flush=True)
                log.flush()
                next_report = now + interval

    elapsed = int(time.monotonic() - started)
    print(f"Command finished after {elapsed}s (exit {process.returncode})", flush=True)
    if process.returncode:
        print("Compiler errors and final output:")
        for line in dict.fromkeys([*errors, *tail]):
            print(f"  {line[:500]}")
    return process.returncode


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interval", type=int, default=60)
    parser.add_argument("log", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.interval < 1 or not args.command:
        parser.error("an interval of at least one second and a command are required")
    raise SystemExit(run_sdk_command(args.log, args.command, args.interval))
