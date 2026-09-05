#!/bin/sh
set -eu

service=${1:?usage: e1-doom-boot-host-test.sh SERVICE}
work=$(mktemp -d "${TMPDIR:-/tmp}/e1-doom-boot-test.XXXXXX")
cleanup()
{
    if test -s "$work/boot/launcher.pid"; then
        kill "$(cat "$work/boot/launcher.pid")" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM

card=$work/card
runtime=$work/runtime
boot_state=$work/boot
mkdir -p "$card/e1-doom-flat/bin" "$card/e1-doom-flat/share/doom"
touch "$card/.e1-doom-autostart-enabled" "$work/device.ready"
touch "$card/e1-doom-flat/bin/e1-doom-controller"
touch "$card/e1-doom-flat/bin/e1-doom"
chmod +x "$card/e1-doom-flat/bin/e1-doom-controller"
chmod +x "$card/e1-doom-flat/bin/e1-doom"
printf doom >"$card/e1-doom-flat/share/doom/doom1.wad"

launcher=$card/e1-doom-run
# The single-quoted lines are the contents of the generated fixture.
# shellcheck disable=SC2016
printf '%s\n' '#!/bin/sh' \
    'mkdir -p "$E1_DOOM_TEST_RUNTIME/state"' \
    'echo $$ >"$E1_DOOM_TEST_RUNTIME/state/launcher.pid"' \
    'echo camera-armed >"$E1_DOOM_TEST_RUNTIME/state/runtime.active"' \
    'echo camera-armed >"$E1_DOOM_TEST_RUNTIME/state/runtime.mode"' \
    'trap '\''rm -rf "$E1_DOOM_TEST_RUNTIME"; exit 0'\'' TERM INT HUP' \
    'while :; do sleep 1; done' >"$launcher"
chmod +x "$launcher"

run_service()
{
    E1_DOOM_CARD="$card" \
    E1_DOOM_RUNTIME="$runtime" \
    E1_DOOM_BOOT_STATE="$boot_state" \
    E1_DOOM_LAUNCHER="$launcher" \
    E1_DOOM_ROOT="$card/e1-doom-flat" \
    E1_DOOM_DEVICE_READY_FILE="$work/device.ready" \
    E1_DOOM_TEST_PIDFILE_ONLY=1 \
    E1_DOOM_TEST_RUNTIME="$runtime" \
        /bin/sh "$service" "$@"
}

run_service start >/dev/null
count=0
while test "$count" -lt 50 && ! test -f "$runtime/state/runtime.active"; do
    sleep 0.1
    count=$((count + 1))
done
test -f "$runtime/state/runtime.active"
run_service status | grep -q 'mode=camera-armed'
run_service start | grep -q 'already active'
run_service stop | grep -q 'camera runtime is stock'
test ! -e "$runtime"

touch "$card/.e1-compatibility-reserved"
run_service start >/dev/null
count=0
while test "$count" -lt 50 && ! test -f "$runtime/state/runtime.active"; do
    sleep 0.1
    count=$((count + 1))
done
test -f "$runtime/state/runtime.active"
test ! -e "$card/.e1-compatibility-reserved"
test -e "$card/.e1-compatibility-reserved.paused-by-e1-doom-autostart"
run_service stop >/dev/null

rm -f "$card/.e1-doom-autostart-enabled"
touch "$card/.e1-compatibility-reserved"
if run_service start >/dev/null 2>&1; then
    echo "service started without its explicit enable marker" >&2
    exit 1
fi
test -e "$card/.e1-compatibility-reserved"

echo "E1 Doom boot-service host test passed"
