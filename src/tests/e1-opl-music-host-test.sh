#!/bin/sh
set -eu

binary=$1
iwad=$2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/e1-doom-opl-test.XXXXXX")

cleanup()
{
    rm -rf "$scratch"
}
trap cleanup EXIT HUP INT TERM

mkdir "$scratch/config"
E1_DOOM_STATE="$scratch/config" \
E1_DOOM_AUDIO="$scratch/audio.pcm" \
E1_DOOM_FRAMEBUFFER="$scratch/framebuffer.shm" \
E1_DOOM_PTZ_SOCKET="$scratch/ptz.sock" \
E1_DOOM_HTTP_PORT=0 \
E1_DOOM_HEIGHT=200 \
E1_DOOM_RUN_SECONDS=5 \
E1_DOOM_TEST_SFX=3 \
E1_DOOM_TEST_SFX_DELAY_MS=500 \
"$binary" -iwad "$iwad" -warp 1 1 >"$scratch/game.log" 2>&1 &
game_pid=$!

# Stand in for the stock AAC callback.  Acknowledge one chronological block
# every 64 ms so the demand-paced mixer keeps advancing during the host test.
python3 - "$scratch/audio.pcm" <<'PY'
import mmap
import os
import struct
import sys
import time

path = sys.argv[1]
deadline = time.monotonic() + 2
while (not os.path.exists(path) or os.path.getsize(path) < 16500):
    if time.monotonic() >= deadline:
        raise SystemExit("audio ring was not created")
    time.sleep(0.01)

with open(path, "r+b", buffering=0) as stream:
    ring = mmap.mmap(stream.fileno(), 16500)
    consume_deadline = time.monotonic() + 4.5
    while time.monotonic() < consume_deadline:
        published, consumed = struct.unpack_from("<2I", ring, 24)
        if consumed < published:
            struct.pack_into("<I", ring, 28, consumed + 1)
            ring.flush()
        time.sleep(0.064)
    ring.close()
PY

wait "$game_pid"

grep -q 'E1_MUSIC init=1 backend=opl2 rate=16000' "$scratch/game.log"
grep -q 'E1_MUSIC registered format=MUS (OPL)' "$scratch/game.log"
grep -q 'E1_MUSIC playing loop=1' "$scratch/game.log"
sfx_starts=$(grep -c '^E1_AUDIO_TEST_SFX pistol handle=[0-9]' \
    "$scratch/game.log" || true)
if test "$sfx_starts" -ne 3; then
    grep '^E1_AUDIO\|^E1_MUSIC' "$scratch/game.log" >&2 || true
    exit 1
fi

python3 - "$scratch/framebuffer.shm" <<'PY'
import struct
import sys

framebuffer = open(sys.argv[1], "rb").read()
magic, version, size, active, frame_sequence = struct.unpack_from(
    "<5I", framebuffer, 0
)
sequence, width, height, pitch = struct.unpack_from("<4I", framebuffer, 32)
palette = framebuffer[48:816]
pixels = framebuffer[816:816 + 428 * 200]
assert magic == 0x4531444D
assert version == 1
assert size == len(framebuffer)
assert active == 0
assert frame_sequence > 0
assert sequence > 0 and sequence % 2 == 0
assert (width, height, pitch) == (426, 200, 428)
assert any(palette)
assert any(pixels)
print(
    f"e1-zero-copy-framebuffer-test-ok frames={frame_sequence} "
    f"sequence={sequence} geometry={width}x{height}"
)
PY

python3 - "$scratch/audio.pcm" <<'PY'
import struct
import sys

fields = struct.unpack("<13I", open(sys.argv[1], "rb").read(52))
magic, version, size, rate, samples, slots = fields[:6]
published, consumed, drops, mixed, nonzero, last_nonzero, peak = fields[6:]
assert magic == 0x45314155
assert version == 4
assert size == 16500
assert rate == 16000
assert samples == 1024
assert slots == 8
assert published == mixed
assert published - consumed == 3
assert mixed >= 25
assert nonzero >= 20
assert last_nonzero > 0
assert peak > 0
print(
    f"e1-opl-music-host-test-ok mixed={mixed} nonzero={nonzero} "
    f"peak={peak} drops={drops}"
)
PY
