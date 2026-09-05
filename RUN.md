# Running E1 Doom

E1 Doom targets the Reolink E1 Zoom E340 (`IPC_NT14NA48MPSD6`). It was developed against `v3.2.0.4741_2503281992` and a later `v3.2.0.0` build. Semantic signatures permit known-compatible code relocation and refuse an absent or ambiguous media, PTZ, or audio interface before writing a patch.

Root access and a way to copy files to the SD card are prerequisites. This project does not provide either one.

## Install the payload

Build as described in [README.md](README.md), then place the outputs here:

```text
/mnt/sda/e1-doom-flat/  <- build/e1-doom-root/
/mnt/sda/e1-doom-run   <- build/e1-doom-run
/mnt/sda/e1-doom-boot  <- build/e1-doom-boot
```

The boot service is deliberately separate from any persistent-root bootstrap. If you choose to invoke it at boot, create `/mnt/sda/.e1-doom-autostart-enabled` and use `/bin/sh /mnt/sda/e1-doom-boot start`. It makes one bounded, firmware-validated attempt and leaves Doom stopped in `camera-armed` mode. The direct launcher refuses an existing `/mnt/sda/.e1-compatibility-reserved` marker; the enabled boot service treats its own marker as an explicit ownership handoff and parks the reservation. Do not enable it on a shared camera without approval.

Check the listener with:

```sh
/bin/sh /mnt/sda/e1-doom-boot status
```

The normal result is `active ... mode=camera-armed`.

If port 666 disappears after a reboot even though the payload and enable marker remain on the SD card, check the separate persistent-root bootstrap first. An update or another camera project can preserve every E1 Doom file while replacing the one local hook that invokes `e1-doom-boot`. Reapply that integration hook and run the status command above; there is no reason to rebuild or recopy the Doom payload. This repository intentionally does not supply or modify the bootstrap itself.

## Start and play

Open the camera's Fluent stream in the Reolink app. Start Doom from the port-666 dashboard or enter Up, Up, Down, Down, Left, Right, Left, Right on the PTZ pad. Some app versions send short taps as directionless stop events; E1 Doom recognizes the eight timed stop events as the same toggle. Held PTZ directions continue to work, and double-tap Up is the toy Fire+Use/Confirm gesture.

Run `tools/e1-controller-url` on the development machine and open the printed URL from a browser on the same LAN. Keep the page focused and use:

- W/A/S/D: move and strafe.
- Arrow keys: move and turn.
- Ctrl: fire; Space: use; Shift: run; 1–9: weapons.
- Escape: menu/back; Enter: select; Tab: map; F2/F3: save/load.

The page releases held keys when it loses focus, closes, is hidden, or misses its heartbeat. It contains explicit Start Doom, Return to Camera, and Release Keys controls.

Doom opens on the original title screen. The first attract demo waits until about 20 seconds after that title has become fully visible; initialization and the entry melt do not consume the delay.

The Home Hub or phone app may mute playback locally. Enable listening or Tap to Talk to hear Doom. PTZ UI interactions can mute the app again even while the camera's AAC stream remains healthy.

## Stop safely

Return to the camera from the dashboard, Doom's Quit Game menu, the same eight-tap PTZ sequence, or a Down hold of a little over three seconds. The runtime stops music and effects, plays `pldeth` alone during the exit melt, restores the microphone, removes the hooks, and terminates Doom. The port-666 controller remains armed for another run.

Stop the lightweight listener itself with:

```sh
/bin/sh /mnt/sda/e1-doom-boot stop
```

Do not leave a live injected runtime unattended. A completed run must have all three properties:

```sh
test ! -e /mnt/tmp/e1-doom
device_pid=$(/bin/pidof device)
! grep -q e1-doom-injected.so "/proc/$device_pid/maps"
```

The Home Hub or Reolink client must also have re-established its ordinary camera session. If recovery is incomplete, the launcher deliberately retains `/mnt/tmp/e1-doom` for inspection instead of pretending cleanup succeeded.

## Probe after a firmware update

Run the non-mutating compatibility check before any live test:

```sh
E1_DOOM_PROBE_ONLY=1 /bin/sh /mnt/sda/e1-doom-run
```

This validates the current executable's loader slots, PTZ/audio hook sites, direct actuator ABI, and statically linked HDAL wrappers. It does not load the injection library or change camera behavior.

For a live development run, start `tools/e1-safety-monitor 180` on the host first. It records the camera boot identity, hardware-watchdog refresh, stock process IDs, encoder paths, and critical thread state, and requests software recovery when those signals deteriorate.

## Storage and iteration

The SD payload is an ordinary directory. The WAD, Woof data, executables, and scripts live there; configuration, frame/audio buffers, sockets, logs, and PID files live in `/mnt/tmp/e1-doom`. During tuning, replace only the changed executable while the runtime is stopped. There is no SquashFS image and no reason to resend the WAD.
