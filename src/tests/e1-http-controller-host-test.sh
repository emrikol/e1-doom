#!/bin/sh
# shellcheck disable=SC2251
set -eu

binary=$1
iwad=$2
controller=$3
port=18666
scratch=$(mktemp -d "${TMPDIR:-/tmp}/e1-doom-http-test.XXXXXX")
game_pid=
controller_pid=

cleanup()
{
    if test -n "$game_pid" && kill -0 "$game_pid" 2>/dev/null; then
        kill -TERM "$game_pid" 2>/dev/null || true
        wait "$game_pid" 2>/dev/null || true
    fi
    if test -n "$controller_pid" && kill -0 "$controller_pid" 2>/dev/null; then
        kill -TERM "$controller_pid" 2>/dev/null || true
        wait "$controller_pid" 2>/dev/null || true
    fi
    rm -rf "$scratch"
}
trap cleanup EXIT HUP INT TERM

mkdir "$scratch/config"
printf '%s\n' camera-armed >"$scratch/runtime.mode"
E1_DOOM_RUNTIME_STATE="$scratch" \
E1_DOOM_PTZ_SOCKET="$scratch/ptz.sock" \
E1_DOOM_HTTP_PORT="$port" \
"$controller" >"$scratch/controller.log" 2>&1 &
controller_pid=$!

attempt=0
while test "$attempt" -lt 50; do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" \
        >"$scratch/health" 2>/dev/null; then
        break
    fi
    kill -0 "$controller_pid"
    sleep 0.1
    attempt=$((attempt + 1))
done
test "$attempt" -lt 50
grep -qx ok "$scratch/health"
test -f "$scratch/controller.ready"

curl -fsS "http://127.0.0.1:$port/" >"$scratch/controller.html"
grep -q 'DOOM REMOTE' "$scratch/controller.html"
grep -q 'START DOOM' "$scratch/controller.html"
grep -q 'RETURN TO CAMERA' "$scratch/controller.html"
grep -q 'orientation:portrait' "$scratch/controller.html"
grep -q 'viewport-fit=cover' "$scratch/controller.html"
test "$(curl -fsS "http://127.0.0.1:$port/mode")" = camera-armed
curl -fsS -o /dev/null "http://127.0.0.1:$port/start"
test -f "$scratch/runtime.start"

E1_DOOM_STATE="$scratch/config" \
E1_DOOM_RUNTIME_STATE="$scratch" \
E1_DOOM_AUDIO="$scratch/audio.pcm" \
E1_DOOM_FRAMEBUFFER="$scratch/framebuffer.shm" \
E1_DOOM_PTZ_SOCKET="$scratch/ptz.sock" \
E1_DOOM_HTTP_PORT=0 \
E1_DOOM_HEIGHT=300 \
E1_DOOM_RUN_SECONDS=12 \
"$binary" -iwad "$iwad" >"$scratch/game.log" 2>&1 &
game_pid=$!

attempt=0
while test "$attempt" -lt 50; do
    if test -f "$scratch/game.ready" && test -S "$scratch/ptz.sock"; then
        break
    fi
    kill -0 "$game_pid"
    sleep 0.1
    attempt=$((attempt + 1))
done
test "$attempt" -lt 50
grep -q "^pid=$game_pid frame=[1-9][0-9]*$" "$scratch/game.ready"
sleep 0.2
! grep -q 'E1_TITLE visible-start' "$scratch/game.log"
printf '%s\n' doom >"$scratch/runtime.mode"
attempt=0
while test "$attempt" -lt 20 &&
      ! grep -q 'E1_TITLE visible-start' "$scratch/game.log"; do
    sleep 0.1
    attempt=$((attempt + 1))
done
test "$attempt" -lt 20

# Exercise the real Doom menu/S_StartSound path.  A direct platform mixer
# call is insufficient: snd_channels must also be initialized or Woof rejects
# the sound before it reaches I_StartSound().
curl -fsS -o /dev/null "http://127.0.0.1:$port/down/Escape"
curl -fsS -o /dev/null "http://127.0.0.1:$port/up/Escape"
sleep 0.2
curl -fsS -o /dev/null "http://127.0.0.1:$port/down/ArrowDown"
sleep 0.2
curl -fsS -o /dev/null "http://127.0.0.1:$port/up/ArrowDown"
sleep 0.2
grep -q 'E1_AUDIO_SFX .*name=pstop ' "$scratch/game.log"

curl -fsS -o /dev/null "http://127.0.0.1:$port/camera"
test -f "$scratch/dashboard.camera"

curl -fsS -o /dev/null "http://127.0.0.1:$port/down/KeyW"
curl -fsS -o /dev/null "http://127.0.0.1:$port/up/KeyW"
curl -fsS -o /dev/null "http://127.0.0.1:$port/down/ControlLeft"
curl -fsS -o /dev/null "http://127.0.0.1:$port/up/ControlLeft"
curl -fsS -o /dev/null "http://127.0.0.1:$port/down/KeyD"
sleep 3

printf '%s\n' exit-melt >"$scratch/runtime.mode"
attempt=0
while test "$attempt" -lt 30 &&
      ! grep -q 'E1_EXIT_AUDIO started sfx=pldeth' "$scratch/game.log"; do
    sleep 0.1
    attempt=$((attempt + 1))
done
test "$attempt" -lt 30
grep -q 'E1_AUDIO_SFX .*name=pldeth ' "$scratch/game.log"
grep -q 'E1_EXIT_AUDIO death-only=1' "$scratch/game.log"

kill -TERM "$game_pid"
wait "$game_pid" || true
game_pid=
test ! -e "$scratch/game.ready"
printf '%s\n' camera-armed >"$scratch/runtime.mode"
test "$(curl -fsS "http://127.0.0.1:$port/mode")" = camera-armed
curl -fsS "http://127.0.0.1:$port/health" | grep -qx ok

kill -TERM "$controller_pid"
wait "$controller_pid"
controller_pid=
test ! -e "$scratch/controller.ready"

grep -q "E1_HTTP ready address=0.0.0.0 port=$port deadman_ms=2500" \
    "$scratch/controller.log"
grep -q 'E1_GAME_READY ' "$scratch/game.log"
grep -q 'E1_TITLE visible-start' "$scratch/game.log"
! grep -q 'E1_INPUT startup-menu=escape' "$scratch/game.log"
grep -q 'E1_INPUT http-key=0x1b action=1' "$scratch/game.log"
grep -q 'E1_INPUT http-key=0x1b action=0' "$scratch/game.log"
grep -q 'E1_INPUT http-key=0x77 action=1' "$scratch/game.log"
grep -q 'E1_INPUT http-key=0x77 action=0' "$scratch/game.log"
grep -q 'E1_HTTP key=ControlLeft down' "$scratch/controller.log"
grep -q 'E1_HTTP key=ControlLeft up' "$scratch/controller.log"
grep -q 'E1_HTTP release-all reason=heartbeat-timeout' "$scratch/controller.log"
grep -q 'E1_DASHBOARD request=start source=http' "$scratch/controller.log"
grep -q 'E1_DASHBOARD request=return-to-camera source=http' "$scratch/controller.log"

echo 'e1-http-controller-host-test-ok idle-controller=resident game=on-demand first-frame=ready death-only=pldeth keys=ipc deadman=2500ms'
