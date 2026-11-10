#!/usr/bin/env python3
import os
import sys
import signal
import select
import subprocess
import time

FEEDER = [
    "/opt/cedar-native/venv/bin/python",
    "/opt/cedar-native/beta54/cedar-feed-ass-v5.4.py",
    sys.argv[1],
]

FFMPEG = [
    "/usr/lib/jellyfin-ffmpeg/ffmpeg",
    *sys.argv[2:],
]

children = []
cancelled = False
reason = ""
stdin_open = True


def shutdown(signum, frame):
    global cancelled, reason
    cancelled = True
    reason = signal.Signals(signum).name


signal.signal(signal.SIGTERM, shutdown)
signal.signal(signal.SIGINT, shutdown)


def signal_groups(sig):
    for proc in reversed(children):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            pass


def cleanup():
    signal_groups(signal.SIGTERM)

    deadline = time.monotonic() + 3.0

    while time.monotonic() < deadline:
        if all(p.poll() is not None for p in children):
            break
        time.sleep(0.1)

    # Limpa tambem descendentes que sobreviveram
    # ao encerramento do lider do grupo.
    signal_groups(signal.SIGKILL)

    for proc in children:
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


def check_stdin():
    global stdin_open, cancelled, reason

    if not stdin_open:
        return

    try:
        ready, _, _ = select.select([sys.stdin], [], [], 0)
        if not ready:
            return

        data = os.read(sys.stdin.fileno(), 4096)

        if not data:
            stdin_open = False
            return

        if b"q" in data.lower():
            cancelled = True
            reason = "JELLYFIN_QUIT"

    except (OSError, ValueError):
        stdin_open = False


env = os.environ.copy()
env["CEDAR_STREAM_TS"] = "1"
env.setdefault("CEDAR_MAX_PACKETS", "0")
env.setdefault("CEDAR_TIMEOUT", "180")

result = 1

try:
    feeder = subprocess.Popen(
        FEEDER,
        stdout=subprocess.PIPE,
        env=env,
        start_new_session=True,
    )
    children.append(feeder)

    ffmpeg = subprocess.Popen(
        FFMPEG,
        stdin=feeder.stdout,
        start_new_session=True,
    )
    children.append(ffmpeg)

    feeder.stdout.close()

    while True:
        check_stdin()

        if cancelled:
            print(
                "CEDAR_CONTROL_STOP=" + reason,
                file=sys.stderr,
                flush=True,
            )
            result = 0 if reason == "JELLYFIN_QUIT" else 143
            break

        rc = ffmpeg.poll()

        if rc is not None:
            result = rc
            break

        # Evita permanecer ativo caso o feeder morra
        # e o FFmpeg nao encerre normalmente.
        frc = feeder.poll()
        if frc is not None and frc != 0:
            print(
                "CEDAR_FEEDER_EXIT=" + str(frc),
                file=sys.stderr,
                flush=True,
            )
            result = 1
            break

        time.sleep(0.1)

finally:
    cleanup()

# Verificacao final dos dois processos.
# Nao aceitar sucesso se o feeder terminou com erro.
if not cancelled:
    feeder_rc = feeder.returncode
    ffmpeg_rc = ffmpeg.returncode

    print(
        f"CEDAR_FINAL feeder={feeder_rc} ffmpeg={ffmpeg_rc}",
        file=sys.stderr,
        flush=True,
    )

    if feeder_rc != 0 or ffmpeg_rc != 0:
        result = 1

sys.exit(result)
