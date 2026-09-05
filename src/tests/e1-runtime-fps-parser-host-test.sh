#!/bin/sh
set -eu

runtime=${1:-target/e1-doom-runtime}
fixture=$(mktemp)
trap 'rm -f "$fixture"' EXIT HUP INT TERM

write_fixture()
{
    fps=$1
    cat >"$fixture" <<EOF
------------------------- VIDEOENC 0  RC ---------------------------------------
out   mode  swmode  gop  bitrate  fr     I(int/min/max)
0     EVBR  0       40   4194304  20 / 1  ( 35/ 25/ 51)
1     EVBR  0       40   1257472  20 / 1  ( 26/ 20/ 40)
2     EVBR  0       40   262144   $fps / 1  ( 26/ 20/ 40)
3     EVBR  0       20   262144   10 / 1  ( 26/ 20/ 40)
EOF
}

parse_fixture()
{
    E1_DOOM_FPS_PARSE_ONLY=1 E1_DOOM_VENC_INFO="$fixture" \
        /bin/sh "$runtime"
}

write_fixture 10
test "$(parse_fixture)" = 10

# Prove that column six is consumed rather than the adjacent bitrate or a
# hard-coded fallback.
write_fixture 15
test "$(parse_fixture)" = 15

# Implausible and malformed values retain the bounded 10 FPS fallback.
write_fixture 31
test "$(parse_fixture)" = 10
write_fixture invalid
test "$(parse_fixture)" = 10

echo e1-runtime-fps-parser-host-test-ok
