# E1 Zoom DOOM — complete implementation writeup

Status: implemented, physically demonstrated in the Reolink app, performance
qualified, and restored to the stock camera entirely in software.

This is the canonical description of the code published here. It describes
the final flat-SD, 426x200, four-direction/PTZ-plus-browser, OPL2 design rather
than the discarded prototypes that preceded it.

## 1. What the finished proof of concept does

One SD-backed persistent-root service turns the camera into an armed Doom
appliance while leaving the game and all live mutation on demand:

- The armed state keeps the port-666 dashboard and guarded PTZ/audio hooks
  available, but does not run the Doom engine, renderer, or synthesizer.
- The normal Reolink live view remains the display. Doom covers the complete
  640x360 Fluent viewport before the camera's existing H.264 hardware encoder.
- The official, unmodified Doom v1.9 shareware IWAD supplies the levels,
  sprites, sounds, music data, menus, and demons.
- Doom music and sound effects replace microphone PCM immediately before the
  stock AAC encoder. The stock codec, stream writer, timestamps, RTSP/P2P path,
  and Reolink app remain in use.
- The four usable Reolink PTZ directions are a fallback controller. A small
  HTTP controller on port 666 turns a browser keyboard into a full Doom
  controller, including menus.
- A start request launches Doom and waits for its first complete frame before
  the Doom-style entry melt. Exit plays the Doom player-death sound over the
  column melt, returns to a fresh camera image and microphone audio, and reaps
  the game without disconnecting the viewer or port-666 controller.
- Stopping the boot service removes the hooks, injected library, HTTP listener,
  processes, sockets, frame/audio buffers, config, and tmpfs state. Nothing is
  retained in the live camera runtime and no camera setting is changed.

The normal status/launch controls are:

```sh
/bin/sh /mnt/sda/e1-doom-boot status
/bin/sh /mnt/sda/e1-doom-boot start
/bin/sh /mnt/sda/e1-doom-boot stop
```

Run `tools/e1-controller-url` to print the browser controller URL for the
camera's current DHCP address. See `RUN.md` for the short operating procedure.

## 2. Target and compatibility scope

The implementation is intentionally tied to one camera model and ARM/HDAL ABI,
but compatible firmware builds may relocate the code it uses:

| Item | Current target |
| --- | --- |
| Camera | Reolink E1 Zoom E340 |
| Hardware type | `IPC_NT14NA48MPSD6` |
| Tested firmware | `v3.2.0.4741_2503281992`; later build reporting `v3.2.0.0` |
| Tested `/mnt/app/device` MD5 | `8afd998456cfa148051689a9c1089380`; `186ebe9a79115b4137367e015d2dd18e` |
| CPU | Two ARMv7 Cortex-A7 processors, NEON/VFPv4, up to 800 MHz |
| ABI | 32-bit ARM EABI5 hard-float, glibc userspace |
| Stock game stream | encoder 2, 640x360 H.264 Fluent path |
| Stock audio | MPEG-2 AAC-LC, 16 kHz, mono, 1,024 PCM samples per AAC frame |

It is intentionally a toy, not a portable camera package. Loader relocations,
exact and masked instruction signatures, function ordering, mapping
permissions, stream numbers, and the unchanged shared-library ABI are checked
because guessing them on another firmware would be unsafe. Hook addresses and
the direct PTZ actuator ABI are discovered at runtime. A compatible firmware
update can move them; missing, duplicate, or implausibly ordered matches fail
before the camera is patched. There is no authentication on port 666, no
general service manager, no public network support, and no attempt at production
hardening.

## 3. End-to-end architecture

```text
 Reolink app PTZ ---- stock Baichuan path ----> device PTZ hook
                                                    |
                                      camera mode: observe/pass through
                                      Doom mode: consume + send event
                                                    |
                                                    v
 browser keyboard ---> standalone HTTP :666 controller
                                      |
                           Unix datagram key events
                                      |
                                      v
                                  Woof input

 Woof 426x200 indexed frame + PLAYPAL
                 |
       tmpfs shared framebuffer
                 |
                 v
 injected e1-doom-osg worker
   indexed -> 640x360 ARGB4444 full-frame OSG
                 |
                 v
 stock HDAL encoder 2 -> stock stream buffers -> RTSP/P2P -> Reolink app

 Woof DMX SFX + MUS/MIDI -> custom mixer + OPL2
                 |
       16 kHz mono tmpfs ring
                 |
                 v
 stock device audio hook -> existing AAC encoder -> existing audio stream
```

There is no Doom RTSP server and no software H.264 or AAC encoder. The only
HTTP server is a small standalone controller. It carries input and lifecycle
requests, serves no video or audio, and remains available while the game is
stopped.

## 4. Woof and Doom customizations

### 4.1 Upstream and game data

The engine is Woof 15.3.0 at upstream commit
`9f124c6269926f90231592ea1bd2c39358ad17c9`. `make deps` fetches that exact
revision into `.deps/woof` and applies `patches/woof/e1-doom.patch`. Woof is
predominantly GPL-2.0-or-later and includes GPL-3.0-or-later files; the linked
E1 Doom executable is distributed under GPL-3.0-or-later.

The IWAD is the official Doom v1.9 shareware `doom1.wad`:

- Size: 4,196,020 bytes
- MD5: `f0cefca49926d00903cf57551d901abe`
- SHA-256: `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`

It is stored once on the SD card and is not copied into tmpfs for each run.
`woof.pk3` supplies Woof's engine resources.

### 4.2 Native camera platform instead of desktop SDL

`src/platform/e1/` replaces Woof's desktop platform layer:

- `i_main_e1.c` selects the shareware IWAD, installs clean signal handling,
  adds `-nogui`, and starts ordinary `D_DoomMain()`.
- `i_video_e1.c` provides a headless indexed framebuffer publisher and a
  monotonic, stream-rate presentation clock.
- `i_input_e1.c` builds in two modes: Doom receives local PTZ/browser datagrams,
  while the standalone controller build owns HTTP, browser key tracking,
  lifecycle endpoints, and stuck-key recovery.
- `i_sound_e1.c` implements Doom DMX sound effects, OPL2 music, the fixed-rate
  mixer thread, and the shared PCM ring.
- `i_timer_e1.c` implements Woof's timers from `CLOCK_MONOTONIC`.
- `i_system_e1.c`, `platform_stubs_e1.c`, and the small compatibility headers
  provide the required camera-safe system surface while disabling windows,
  OpenGL, OpenAL, SDL input, gamepads, gyro, rumble, and multiplayer UI.

The production ELF has no SDL, OpenAL, ALSA, FluidSynth, or window-system
dependency. The game opens only its local Unix datagram input socket; the
standalone controller owns TCP port 666. Woof multiplayer networking is
stubbed out.

### 4.3 Menu and lifecycle behavior

The E1 build keeps the normal Doom main and in-game menus but hides or skips
desktop-only setup entries whose configuration variables are not compiled.
The setup reset pass is disabled for the camera target.

The engine begins on the official shareware `TITLEPIC` rather than warping into
E1M1 or manufacturing an Escape key to cover it with the menu. The E1-specific
title dwell uses a 20-second monotonic wall clock rather than assuming the
camera-paced game loop advances page tics at exactly 35 Hz. Its countdown is
paused during cold initialization and the external entry melt, so the user gets
roughly 20 seconds after the camera has reached fully visible `doom` mode before
`DEMO1` starts. Escape still opens the ordinary main menu immediately.

Selecting `Quit Game -> Yes` no longer performs Woof's immediate process exit;
it writes the same return-to-camera request as the dashboard. The coordinator
lets the exit melt finish, restores stock audio, reaps the game, and remains
armed for another later start.

All generated config and any save-related files are rooted under
`/mnt/tmp/e1-doom/state/config`. The proof of concept does not preserve them.

### 4.4 True-widescreen rendering and the status-bar fixes

The accepted default renders 426x200 indexed pixels. This is not a stretched
320x200 view: Woof's logical viewport remains 426x200 with
`video.unscaledw = 426`, correct aspect handling enabled, and the widescreen
delta derived from the original 320-wide playfield. That preserves the proper
field of view, weapon placement, and status-bar geometry.

Rows are aligned to four bytes, so a 426-pixel frame has a 428-byte pitch. The
published pitch is carried explicitly; treating it as 426 caused the diagonal
skew seen in the Reolink screenshot and was fixed. A 640x300 quality/debug
render mode remains available, but the runtime default is 426x200.

The camera compositor performs nearest-neighbor mapping from the 426x200
logical image to the complete 640x360 OSG surface. This simultaneously applies
the intended widescreen horizontal scale and Doom's display pixel-aspect
correction. It does not letterbox, crop, or leave a floating weapon/status bar.

### 4.5 Doom renderer CPU changes

The upstream renderer is retained, but its flat/span inner loop is specialized
for the ARM camera build:

- texture, brightmap, colormap, fraction, and step values are copied to local
  restricted variables so the compiler keeps them in registers;
- the loop handles two pixels per iteration;
- a small ARM post-index `strb` sequence advances the destination without the
  compiler repeatedly materializing its address;
- the final fractional coordinates are written back once per span.

The output scaler is also handwritten ARMv7 assembly. It processes four
indexed pixels per iteration, performs four palette lookups, packs two
ARGB4444 pixels per word with `pkhbt`, and stores two words. The indexed lookup
is a gather workload, so this accepted kernel uses ARM integer registers rather
than pretending NEON has an efficient arbitrary 256-entry gather. The build
still targets NEON/VFPv4 and permits compiler-generated NEON where useful.

## 5. Video path

### 5.1 Producer

Woof renders directly into the pixel array of a tmpfs shared-memory
framebuffer. The producer uses an odd/even sequence protocol: odd means the
sole zero-copy slot is being drawn, even means its dimensions, 428-byte pitch,
palette, and pixels are stable. A separate monotonically increasing frame
sequence lets the consumer count missed source frames.

The game clock remains the normal Doom 35 Hz clock. Presentation is capped to
the selected camera stream's configured frame rate rather than rendering work
that H.264 cannot carry. The launcher reads encoder 2's live FPS column from
`/proc/hdal/venc/info`, validates 1–30 FPS, and supplies it to both producer and
consumer; 10 FPS is the fallback.

### 5.2 Consumer and hardware encoding

`e1-doom-injected.so` runs an `e1-doom-osg` worker inside the stock `device`
process so it can use the owning HDAL context. It validates the shared-memory
header, dimensions, pitch, and stable sequence, then:

1. converts the current PLAYPAL palette to opaque ARGB4444;
2. maps 426 columns to 640 using a precomputed `x_map`;
3. maps 200 rows to 360 using a precomputed `y_map`;
4. copies already-expanded rows when multiple output rows select the same
   source row;
5. publishes a 640x360, full-opacity, top-left OSG stamp on encoder 2.

The worker polls twice per output frame interval. If it catches the producer
while the sequence is odd, it retries without advancing the presentation
deadline. This avoids phase locking two 10 Hz loops and losing one stable frame
per cycle. Source frames, source drops, publishes, HDAL errors, retries, and
last sequence are continuously exposed in `state/video.stats`.

HDAL still performs H.264 encoding. The existing stock stream-buffer fan-out,
RTSP server, Home Hub/P2P path, authentication, and Reolink application are not
replaced.

### 5.3 Entry and exit melts

The melt is a deterministic 320-column state machine advancing at Doom's
35 Hz timing independent of the 10 Hz H.264 presentation. During entry, each
column changes from transparent camera to the current Doom frame. During exit,
a snapshot of Doom drops away to transparency, revealing a fresh live camera
image beneath it. Completion removes the OSG path entirely rather than leaving
a transparent allocation behind.

Entry has a strict readiness gate. Doom atomically publishes `game.ready` with
its PID and nonzero frame sequence only after WAD, audio, and input startup and
the first completed framebuffer publication. The coordinator additionally
requires the input socket, framebuffer, PCM ring, and initialization log
markers, enables AAC substitution, and only then asks the injected worker to
begin the melt. If any requirement is missing after 15 seconds or the game
exits early, it reaps the child and returns to `camera-armed` without obscuring
the camera.

At exit-melt start, the game stops music and every active effect, enables a
death-only mixer guard, and starts the original `pldeth` effect. No later game
sound or music can replace it. The Doom PCM path stays connected until the melt
has completely revealed the camera; the coordinator then restores microphone
PCM and terminates the game.

## 6. Audio path

### 6.1 Doom sound-effects mixer

The camera backend parses Doom's original unsigned 8-bit DMX lumps and mixes
up to 16 voices. Each voice has fixed-point sample position, pitch-adjusted
step, volume, pause, and active state. Linear interpolation converts source
rates to 16 kHz. Because the destination is mono, the complementary stereo pan
gains are not calculated and then added back together; one mono gain is used.

The E1 platform replaces Woof's desktop `i_sound.c`, which normally initializes
the high-level `snd_channels` setting. The camera backend must therefore set it
to its 16 implemented voices itself. Leaving the zero-initialized value in
place makes Woof reject every real game sound before it reaches the otherwise
working low-level mixer. The HTTP regression now moves the real Doom menu and
requires its `pstop` effect, rather than proving only a direct synthetic mixer
call. Effects default to volume 6 so overlapping monsters and weapons leave
headroom for music.

### 6.2 Music backend

The shipped build uses authentic nine-voice OPL2, not OPL3. Woof's MUS/MIDI
scheduler and register writes are retained, but the production synthesizer is
the block-linear `emu8950` core from `rp2040-doom` commit
`29a453c980918a03e40fc8b69b024e7a3bdb5dc2`.

The E1 configuration removes unused rate conversion, floating-point, timers,
test flags, percussion mode, wave-table mapping, and other paths. It renders
one engine update for each four native OPL samples, in blocks, and converts
that roughly 12.4 kHz internal stream into the camera's 16 kHz output while
retaining the phase remainder between callbacks. The earlier stride-eight
build was cheaper but its roughly 6.2 kHz zero-order-held result could sound
choppy despite perfect ring and AAC cadence. The OPL mono signal is boosted 3×
for the unusually quiet camera/app listen path; measured peaks retain headroom
when music is alone. This remains substantially cheaper than running a general
OPL3 core at 48 kHz and resampling afterward.

An optimized Nuked-OPL OPL2 path remains in the tree as an exact reference and
regression target. Its changes include skipping the silent upper OPL3 bank,
fast stationary envelopes, silent operator output, an exact 18-clock noise
jump, and disabled compiler unrolling. It is not the final production
synthesizer.

### 6.3 Mixer cadence and ring

A dedicated `e1-audio` thread is independent of Doom's rendering cadence. It
wakes every 32 ms and maintains a three-frame cushion in an eight-slot shared
ring. Each published frame is exactly 1,024 signed 16-bit mono samples, or
64 ms at 16 kHz. It mixes SFX and music before publication and records producer,
consumer, nonzero, peak, and drop counters.

The consumer begins three frames behind the newest publication for a 192 ms
jitter cushion, reads chronologically, and never blocks the stock audio thread.

### 6.4 Stock AAC substitution

A small Thumb trampoline is installed at the uniquely matched stock final-PCM
encode site (`0x9453c` in the reference build and `0xaa2d8` in the later
build). The hook runs immediately before the existing AAC encoder receives its
normal 2,048-byte PCM block. It is lock-free and bounded:

- in camera/armed mode it leaves microphone PCM untouched;
- in Doom mode it copies the next ring frame into that same buffer;
- on a true underrun it emits silence rather than leaking microphone sound;
- on exit it switches to pass-through, waits for active callbacks to quiesce,
  unmaps the ring, restores the original eight instruction bytes, and verifies
  them.

The Reolink app or Home Hub may still require listening/Tap-to-talk mode before
it plays the camera audio track. That is an app behavior, not a second audio
route. On the current iPhone app, operating the PTZ panel can also mute or
detach local playback even though the camera and Home Hub continue carrying
valid AAC. Re-enable the app's speaker/listen control after PTZ activation, or
start Doom from port 666 to avoid that UI transition.

On 2026-09-05 a live Home Hub capture received 218 consecutive Doom AAC frames
at a mean 63.884 ms cadence, decoded both real SFX and music, and found zero
game-ring underflows, producer drops, or overwritten frames. A music-only
interval before final gain balancing also arrived cleanly at 64.143 ms mean
cadence. These measurements distinguish the former OPL quality/gain issues from
RTSP starvation.

An older diagnostic command that opened a second native AAC path depends on a
private `device` manager-object layout. The production Doom path never uses it;
after firmware relocation it is explicitly disabled until that layout can be
resolved and validated too. This prevents an optional test command from
downgrading the normal runtime's fail-closed guarantee.

## 7. Controls and port 666

### 7.1 PTZ hook and fallback controls

The decoded actuator-dispatch hook site is found from its surrounding
instruction sequence (`0x62db0` in the reference build and `0x68938` in the
later build). At this lower, firmware-stable boundary the direct call ABI is
`r1=channel`, `r2=decoded command`, and `r3=speed`; it does not depend on either
firmware's higher-level request layout or marker. The assembly trampoline
preserves the displaced code and calls a lock-free C router. In
`camera-armed` mode it observes commands for the secret sequence but lets the
original handler move the camera. In Doom mode it consumes supported PTZ
commands so the motor/lens handler does not receive them, then forwards a small
datagram to Woof.

Some Reolink-app taps do not promptly deliver the separate stop command that
the earlier matcher expected. Each nonzero lower-dispatch command is therefore
treated as one complete lifecycle tap; Doom still receives a press and releases
it through the 350 ms input dead-man. The automated Home Hub path has delivered
the full eight-command secret with 180 ms taps through this hook. On 2026-09-05
the physical iPhone app instead delivered eight directionless stop commands;
the fallback counts them as a timed toggle. Eight enter from `camera-armed`;
another eight request the exit melt from Doom.

The current Reolink iPhone UI provides four useful momentary commands:

- Up/Down: forward/back
- Left/Right: turn
- double-tap Up within 700 ms: Fire + Use + Enter together
- eight normal short taps: toggle between the camera and Doom
- hold Down for three seconds: request the exit melt

Input is released on the stock stop command or after a 350 ms timeout. Zoom
IDs remain understood for protocol-level diagnostics, but the app's Zoom
button opens another UI and is not part of the usable controller.

While armed, `U U D D L R L R` within ten seconds (no gap above 1.5 seconds)
requests entry. The HTTP Start Doom button is the normal convenient path.

### 7.2 Browser keyboard controller

`e1-doom-controller` is a small standalone build of the controller half of
`i_input_e1.c`. It is a watched child of the coordinator, listens on
`0.0.0.0:666`, has no external dependencies, and serves one static HTML/CSS/JS
page. It stays alive in `camera-armed` while no Doom process exists. It has
these endpoints:

- `/`, `/health`, and `/mode`
- `/start` and `/camera`
- `/down/<KeyboardEvent.code>` and `/up/<KeyboardEvent.code>`
- `/heartbeat` and `/release-all`

`/start` atomically requests an on-demand launch; `/camera` requests the exit
melt. While the runtime is in Doom mode, the controller sends bounded,
versioned key datagrams to the game's local Unix socket. It converts browser
`KeyboardEvent.code` values to Doom keys for letters,
digits, arrows, modifiers, navigation, punctuation, numpad digits, and F1–F12.
The normal useful set includes WASD, arrows, Ctrl, Space, Shift, 1–9, Escape,
Enter, Tab, and F2/F3.

The server tracks physical browser codes separately from Doom key values, so
left/right modifiers and aliases do not release a key that another code still
holds. Losing focus, hiding/unloading the page, pressing Release Keys, asking
to return to the camera, or missing the 2.5-second browser heartbeat releases
all held inputs.

The controller handles `SIGUSR1` as an unconditional release-all. The
coordinator sends it before reaping a game so held-key state cannot leak into a
later child. A missing game socket merely drops input; it cannot block the HTTP
listener.

The dashboard follows the `DESIGN.md` “1995 deathmatch setup utility” design:
a dependency-free, responsive, square-edged, high-contrast control strip with
explicit CAMERA ARMED/STARTING/transition/DOOM state and Start Doom, Return to
Camera, and Release Keys actions. Landscape viewports retain the three-column
horizontal strip;
any viewport taller than it is wide switches to a centered, single-column
vertical controller even when it is wider than the old phone breakpoint. It
deliberately carries no video; the Reolink app is the screen.

Keyboard stress was tested for 109 seconds with 104 input cycles across the
original reset window. It caused no media loss or reboot. The apparent
keyboard/port-666 freeze correlation was coincidental with the watchdog issue
described below.

## 8. Injection, state machine, and cleanup

### 8.1 Loading the camera-side worker

The launcher first asks the patch supervisor to resolve and inspect the PTZ and
audio sites without stopping any `device` threads. It starts recovery ownership
only after that mutation-free preflight. `e1-doom-injector` then parses the
running firmware's ELF dynamic relocations to find the writable `dlopen`,
`dlsym`, and `dlclose` slots instead of calling fixed PLT/GOT addresses. It
loads `e1-doom-injected.so` into `device`, and unloads through a `dlclose`
address obtained from the resolved `dlsym`.

Inside the library, exact 12-byte prologues resolve the statically linked HDAL
memory, video-encoder, and audio-encoder wrappers. Seventeen must be globally
unique. The audio-stop prologue has a known duplicate, so exactly one candidate
must lie between the independently unique audio-start and audio-close matches.
Memory, video, and audio groups must also retain their expected order and fit
within bounded spans. Only then does the library expose hook addresses or open
HDAL resources. The patch supervisor saves, installs, restores, and
byte-verifies the two eight-byte Thumb patches.

The externally visible runtime state machine is:

```text
CAMERA_ARMED -> STARTING -> ENTRY_MELT -> DOOM
      ^                                  |
      +--------- STOPPING <- EXIT_MELT <-+
```

The controller, injection worker, and recovery supervisors exist throughout
this armed state machine. Doom exists only from `STARTING` through `STOPPING`.
Audio substitution is active only after the readiness gate and through the
exit melt, and returns to the microphone before the game is reaped. The
full-frame OSG exists only from entry through exit. Port 666 therefore remains
available for another cold start without paying the idle cost of the game.

### 8.2 Normal cleanup order

Stopping the launcher completely performs these operations in order:

1. switch audio back to microphone pass-through and quiesce callbacks;
2. return PTZ routing to camera mode;
3. stop the injected worker;
4. restore and verify the PTZ and audio instruction bytes;
5. stop/close the OSG path and free its HDAL media block;
6. wait for the injected thread to exit and unload its shared library;
7. terminate and join the standalone controller and any active Doom child;
8. remove the per-game PID, readiness marker, framebuffer, audio ring, and
   input socket;
9. disarm and wait for the detached runtime watchdog itself to exit;
10. mark restoration complete, then let the outer launcher remove tmpfs and
   the flock lock.

Waiting for the watchdog process—not merely its marker—fixes a race where its
last log/exit write could recreate `/mnt/tmp/e1-doom/state` after the launcher
removed it. That stale directory blocked the next run and made port 666 appear
dead.

### 8.3 Recovery layers

The proof of concept has deliberately redundant recovery because a power cycle
is not an acceptable normal escape:

- The injected worker has a four-second command heartbeat dead-man for HDAL
  media/audio ownership.
- `e1-runtime-watchdog` is detached from the coordinator. If its owner vanishes
  or its heartbeat is stale for 20 seconds, it exact-validates and terminates
  recorded game and controller children, runs the independent PTZ and audio
  restoration paths, and unloads the library.
- Hook-specific supervisors retain the original instruction bytes and will not
  unload code until callbacks are quiescent.
- `tools/e1-safety-monitor` records stock liveness on the host and requests
  recovery if the boot/PID identity changes, an encoder path disappears, or
  the hardware watchdog feed stops refreshing.
- The host resolver pins the camera's SSH key and confirms the E1 Doom payload
  before caching a DHCP address. Optional user-supplied interface MACs narrow
  discovery without embedding a particular camera's identity.

The September 1 freeze was a hardware-watchdog reset. Read-only patch
verification had been stopping all roughly 25 `device` threads with `ptrace`.
Telemetry caught `bc_avencoder`, `bc_audio`, and `bc_osd` in traced-stop state
during cleanup. Inspection now uses read-only `pread()` on
`/proc/<pid>/mem` while the target remains running; only the two actual patch
writes briefly stop threads. That change eliminated the observed reset path.

## 9. Storage and non-persistence

The normal layout is a plain directory on the SD card:

```text
/mnt/sda/e1-doom-run
/mnt/sda/e1-doom-boot
/mnt/sda/e1-doom-flat/bin/
/mnt/sda/e1-doom-flat/share/doom/doom1.wad
/mnt/sda/e1-doom-flat/etc/e1-doom.toml
```

The payload is only a WAD, `woof.pk3`, the executable,
configuration/provenance files, and small helpers. A flat tree allows a
changed binary or script to be copied atomically during tuning without
retransmitting the WAD. The final public build contains no SquashFS path.

All mutable game/runtime data lives in `/mnt/tmp/e1-doom`. Nothing writes MTD,
the U-Boot environment, `/mnt/para`, firmware partitions, crontab, or the
camera configuration database. `target/e1-doom-boot` can be called by an
existing SD-backed root bootstrap, but this repository neither supplies nor
modifies that bootstrap. The service waits for stock `device`, validates the
complete payload and live firmware signatures, and makes only one bounded
attempt. Root acquisition and persistent-root integration are outside the
project.

Per-game objects (`game.pid`, `game.ready`, `ptz.sock`, `framebuffer.shm`, and
`audio.pcm`) are created only for a requested run and removed after every exit.
The separate controller PID/readiness markers live for the lifetime of the
armed boot service.

## 10. Performance work and current measurements

The final performance changes are cumulative:

- cap rendering/presentation to the live H.264 stream FPS;
- render 426x200 rather than 640x300 by default;
- render directly into the shared indexed frame instead of copying a separate
  game framebuffer;
- scale/palette-expand with the four-pixel ARMv7 kernel and reuse duplicate
  output rows;
- keep the Doom span loop's hot state in registers and use post-index stores;
- use a 32-bit sampled once-per-second FNV-1a liveness hash rather than hashing
  every pixel with 64-bit multiplication;
- synthesize nine-voice OPL2 with the reduced block-linear `emu8950` path;
- mix audio only as the stock AAC consumer drains the three-frame cushion;
- poll audio at 32 ms and the stable-frame producer at twice stream cadence;
- compile `-O3` for Cortex-A7 hard-float with frame pointers and build metadata
  omitted.

The stride-eight performance build reached a median 296 scheduler ticks per 30 seconds across
the three owned execution contexts (`e1-doom`, its `e1-audio` thread, and the
injected `e1-doom-osg` thread). At HZ=100 that is 2.96 CPU-seconds per 30
seconds: **9.87% of one core**, or approximately **4.93% of this two-core
camera's total CPU capacity**. The 600-tick target and 650-tick
practical ceiling were both comfortably beaten.

After raising music quality to stride four and enabling the previously blocked
real SFX channels, a live 30-second spot profile measured 337 owned ticks
(172 game, 45 audio, 120 compositor): **11.23% of one core** and still well
inside the original target and ceiling. This is the current quality build; the
296-tick result remains historical evidence for the more aggressive audio
setting rather than a claim about the deployed binary.

The final ten-minute audio cadence run completed 9,375 stock 64 ms reads with
zero underflows, drops, or producer drops. The sustained video gate published
every stable source sequence it observed with zero source skips and zero HDAL
publish errors.

## 11. Build, packaging, and tests

The build uses Zig's cross compiler with target
`arm-linux-gnueabihf.2.34`, CPU `cortex_a7`, hard-float, and NEON/VFPv4. Run:

```sh
make host-check
make check HDAL_INCLUDE_DIR=/path/to/hdal/include
make package HDAL_INCLUDE_DIR=/path/to/hdal/include IWAD=/path/to/doom1.wad
```

`make check` builds the ARM binaries and host tests, verifies ARM EABI5
hard-float flags, checks that the Doom binary retained no desktop media
dependencies, and exercises:

- indexed conversion and the ARM scaler;
- PTZ mapping, secret, hold, timeout, and releases;
- entry/exit melt timing and composition;
- runtime-control transitions;
- 16 kHz AAC/PCM contracts;
- audio-ring ordering, prebuffering, wraparound, underrun, and overrun;
- SFX pitch/resampling/mixing;
- OPL music production and OPL2 fast/reference equivalence;
- standalone-controller HTTP routing, key IPC, key aliases, on-demand lifecycle
  actions, first-frame readiness, death-only exit audio, heartbeat, and
  release-all;
- the live encoder FPS table parser;
- PTZ/audio masked signature resolution, the direct lower PTZ ABI, and all
  HDAL-wrapper exact signatures, including ambiguity and ordering refusal.

Target gates additionally cover exact ABI/signature probes, HDAL allocation and
OSG rollback, changing Doom frames through the stock stream, PTZ/audio patch
restoration, audio tone and Doom PCM substitution, transitions, repeated
cycles, crash recovery, viewer reconnects, sustained load, and post-run stock
state. Those hardware gates were run during development; the public tree keeps
the deterministic host tests and runtime safety tooling rather than the raw
captures and one-off probes.

## 12. Measured integration and firmware-update validation

The keyboard/reset investigation's 109-second integrated run produced:

```text
video: source=921 published=921 source_drops=0 publish_errors=0
audio: frames=1653 dropped=0 underflows=0 producer_drops=0
```

After both cleanup fixes, another integrated run produced 288/288 video frames
and 477 audio callbacks with no drop or underflow. An ordinary launch from the
default flat SD path then produced 116/116 video frames and 235 clean audio
callbacks before restoring stock state.

The original-firmware final audit found:

- unchanged boot ID `8bb7f60f-749a-458f-bc00-ad37bc7e37d5`;
- original stock process IDs still running;
- no Doom/controller process and no port-666 listener;
- no injected mapping, runtime directory, or lock;
- PTZ bytes at `0x636a4`: `70b505460c4686b0`;
- audio bytes at `0x9453c`: `10b500f5cc20002a`;
- live read-only inspection reporting `threads=running`.

On 2026-09-04 a firmware update changed `/mnt/app/device` from MD5
`8afd998456cfa148051689a9c1089380` to
`186ebe9a79115b4137367e015d2dd18e`. The selected lower PTZ site moved from
`0x62db0` to `0x68938`, while the PCM site moved from
`0x9453c` to `0xaa2d8`, all 18 HDAL wrappers moved, and the loader GOT slots
moved. The new semantic/ELF resolvers found both captured builds exactly. The
updated-firmware live gate then produced:

```text
video: source=114 published=114 source_drops=0 publish_errors=0
audio: frames=235 dropped=0 underflows=0 producer_drops=0
```

The dashboard started Doom and returned to the camera. A subsequent SIGTERM of
the outer launcher restored the original PTZ and audio bytes, unloaded the
library, removed every owned process and tmpfs runtime file, kept the same
stock `device` process, and left the hardware watchdog refreshing. The runtime
was then relaunched. The v20 library repeated start/return with 31/31
video frames and 74 audio frames, again with no errors, drops, or underflows,
and was left in `camera-armed` mode with port 666 serving HTTP. The later v24
hook moved interception to the lower actuator dispatcher; an automated
180 ms-tap secret sequence reached all eight decoded directions and entered
Doom. The v25 fallback then accepted the physical iPhone app's eight
directionless stop-only taps and entered Doom. Library v26 made that fallback
symmetric; live owner-v3 tests entered on eight stops and melted back to
`camera-armed` on the next eight.

Library v27 and the standalone controller then moved the game itself to an
on-demand child. One HTTP cycle and one PTZ cycle each cold-started only after a
`frame=1` readiness marker, completed the entry and exit melts, selected only
the `pldeth` exit sound, reaped the game, and left port 666 healthy. A forced
game SIGKILL exercised full recovery without changing device PID or boot ID.
The final armed-idle sample used 96 ticks over 20 seconds across the complete
retained control/recovery stack (4.8% of one core), with no game, framebuffer,
PCM ring, or input socket present.

The final startup adjustment removed the artificial Escape that had covered
`TITLEPIC` with the menu. Its first attract demo is now gated on fully visible
`doom` mode and a monotonic 20-second dwell; the live transition to `DEMO1`
measured 20.099 seconds on the final packaged binary.

## 13. Source map

| Area | Primary files |
| --- | --- |
| Canonical operation | `RUN.md`, this document |
| Build/package | `Makefile`, `sources.lock`, `tools/fetch-sources`, `tools/package-flat` |
| Woof camera platform | `src/platform/e1/` |
| Video/audio/PTZ integration | `src/camera/e1-doom-injected.c`, hook assembly files, `src/common/e1-device-functions.h`, `e1-device-signatures.h` |
| Pixel conversion | `src/common/e1-frame-convert.h`, `e1-frame-expand-arm.S` |
| Audio | `src/common/e1-audio-ring.*`, `e1-sfx-mixer.*`, `patches/rp2040-doom-opl/` |
| Controls/transitions | `src/common/e1-ptz-input.h`, `e1-key-event.h`, `e1-runtime-control.h`, `e1-melt.h` |
| Patch ownership | `src/camera/e1-ptz-patch-supervisor.c` |
| Launch/recovery | `target/e1-doom-run`, `e1-doom-runtime`, `e1-runtime-watchdog`, dead-men/finalizer |
| Host discovery/safety | `tools/e1-resolve`, `tools/e1-ssh`, `tools/e1-scp`, `tools/e1-controller-url`, `tools/e1-safety-monitor`, `tools/e1-live-cpu-profile` |
| Tests | `src/tests/` |

## 14. Known limitations

- It remains E1 Zoom/ARM/HDAL-ABI specific. Compatible firmware relocation is
  accepted only when every semantic signature and ordering check resolves;
  other builds are deliberately refused before mutation.
- Root access must already exist after an update; obtaining or preserving root
  is outside this repository.
- The app exposes only four convenient PTZ gameplay buttons, so the browser is
  the practical full controller. The iPhone does not transmit direction for a
  short tap, so the eight-tap toggle fallback cannot verify the literal
  direction order; held controls still carry real directions.
- Camera/app latency makes it a demonstration more than an ideal way to play.
- App audio may need listening/Tap-to-talk enabled manually.
- Saves/config are volatile by design.
- Port 666 is trusted-LAN-only and unauthenticated.
- The payload expects a writable SD card and a flat directory layout.

Within that scope, the intended joke is complete: video, controls, audio, and
demons all travel through the normal Reolink camera experience, and the camera
returns to stock without a power cycle.
