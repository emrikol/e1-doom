# E1 Doom

E1 Doom is Doom running inside a Reolink E1 Zoom E340. The name was sitting right there.

It replaces the normal 640×360 RTSP picture with a true-widescreen Woof render, feeds 16 kHz OPL2 music and Doom sound effects into the camera's stock audio encoder, and turns PTZ gestures or a tiny browser page on port 666 into a controller. The Reolink app remains the display.

This is a proof of concept, not a camera firmware distribution. Root access is a prerequisite and deliberately out of scope. Nothing here roots the camera, contains firmware, or installs a persistent-root mechanism.

## What is in this repository

- The E1-specific video, audio, input, HTTP controller, injection, lifecycle, and recovery code.
- Small patches against exact Woof and rp2040-doom revisions.
- The final SD-card runtime scripts and host tests.
- Build tooling that fetches the pinned upstream source instead of vendoring it.

There are no compiled binaries, Doom WADs, vendor SDK headers, extracted camera files, Freedoom assets, map tools, profiling captures, or development-history debris.

## Build

You need Git, GNU Make, Python 3, Zig, a C/C++ compiler for host tests, a legally obtained Doom IWAD, and Novatek HDAL headers obtained separately from this repository.

```sh
make deps
make host-check
make HDAL_INCLUDE_DIR=/path/to/hdal/include
make package HDAL_INCLUDE_DIR=/path/to/hdal/include IWAD=/path/to/doom1.wad
```

The package command creates:

```text
build/e1-doom-root/   flat SD-card payload
build/e1-doom-run     guarded launcher
build/e1-doom-boot    lightweight armed service
```

The WAD is copied only into the ignored build directory. It never becomes part of the Git tree.

`make corresponding-source` creates a release companion archive containing this repository plus the fully materialized, patched upstream source. Publish that archive alongside any binary release.

## Run

Copy `build/e1-doom-root` to `/mnt/sda/e1-doom-flat`, and copy the launcher and boot service to `/mnt/sda/e1-doom-run` and `/mnt/sda/e1-doom-boot`. How those files reach a rooted camera, and how a boot hook invokes `e1-doom-boot`, is intentionally left to the reader.

The armed service leaves Doom stopped. Start it from the port-666 dashboard or with the eight-tap PTZ sequence Up, Up, Down, Down, Left, Right, Left, Right. The entry melt waits until Doom has initialized and published its first complete frame. Return to the camera from the dashboard, Doom's quit menu, the same PTZ sequence, or a long Down hold. Exit plays the Doom death sound over the melt, restores stock video/PTZ/microphone audio, and kills the game while leaving the lightweight controller armed.

See [RUN.md](RUN.md) for controls and operational checks, and [IMPLEMENTATION.md](IMPLEMENTATION.md) for the full technical write-up.

## Finding a camera without a fixed address

The host helpers accept an explicit address through `E1_DOOM_HOST`. You can optionally provide a comma-separated set of your camera's interface addresses through `E1_DOOM_MACS`:

```sh
E1_DOOM_HOST=e1-zoom.local tools/e1-controller-url
E1_DOOM_MACS=aa:bb:cc:dd:ee:ff,11:22:33:44:55:66 tools/e1-resolve
```

The resolver verifies the E1 Doom payload and pins the camera's SSH host key in the ignored `.state` directory. No personal address, MAC, key, or WAD hash is built into the repository.

## Safety boundary

The direct launcher refuses a live run when `/mnt/sda/.e1-compatibility-reserved` exists. The boot service additionally requires `/mnt/sda/.e1-doom-autostart-enabled`; it treats that marker as an explicit ownership handoff and parks the compatibility reservation while E1 Doom is enabled. Do not create that marker on a shared camera without the other work's owner approving the handoff. Mutation-free `E1_DOOM_PROBE_ONLY=1` checks remain available. A live run should always end with `/mnt/tmp/e1-doom` absent, `e1-doom-injected.so` absent from the stock `device` process, and the normal camera session restored without a power cycle.

## License

E1 Doom's original code and patches are GPL-3.0-or-later. Upstream components and the user-supplied IWAD retain their own terms; see [THIRD_PARTY.md](THIRD_PARTY.md). No warranty is provided, especially not for experiments involving a rooted camera.
