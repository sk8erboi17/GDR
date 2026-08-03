# GRD

**A low-latency, encrypted remote desktop for fast local networks.**

GRD is a C11 application for streaming and controlling a computer over a LAN. The same
executable can act as a host or a client. Its streaming path is designed around games and
interactive workloads: capture and encoding remain independent from network transmission,
input has its own reliable path, and incomplete video frames are discarded instead of
stalling the pipeline.

GRD currently targets macOS, Windows, and Ubuntu XFCE on X11. It is a LAN-focused project,
not an Internet-facing remote access service.

```text
              GRD in one picture

       HOST                                      CLIENT
  +----------------+                        +----------------+
  | Desktop capture|                        | Display / input|
  +-------+--------+                        +-------+--------+
          |                                         ^
          v                                         |
  +----------------+     encrypted media     +------+---------+
  | Scale + encode | =======================> | Decode + render|
  +-------+--------+       UDP, paced         +----------------+
          ^
          |              encrypted control
  +-------+--------+ <======================= keyboard / mouse
  | Host session   |        reliable TCP      clipboard / state
  +----------------+
```

## Contents

- [Why GRD](#why-grd)
- [Platform support](#platform-support)
- [Quick start](#quick-start)
- [SSH terminal and SFTP](#ssh-terminal-and-sftp)
- [Build from source](#build-from-source)
- [Architecture deep dive](#architecture-deep-dive)
- [Transport and recovery](#transport-and-recovery)
- [Adaptive quality](#adaptive-quality)
- [Input, audio, and session controls](#input-audio-and-session-controls)
- [Configuration and diagnostics](#configuration-and-diagnostics)
- [Optional SFTP and PowerShell access](#optional-sftp-and-powershell-access)
- [Windows CUDA build](#windows-cuda-build)
- [Security model and scope](#security-model-and-scope)

## Why GRD

GRD combines the parts that matter for a responsive LAN session:

- automatic IPv4 multicast/broadcast discovery with active refresh and
  unicast host replies, listing only active hosts;
- one controller and up to three observers;
- H.264, HEVC, and AV1 negotiation with an automatic H.264 fallback;
- negotiated 1080p, 1440p, or 4K streaming up to 120 FPS when the host, client, bitrate,
  and hardware allow it;
- hardware capture, encoding, decoding, and rendering paths on macOS and Windows;
- a dedicated per-client UDP pacer, selective retransmission, and adaptive XOR FEC;
- congestion-aware bitrate and resolution control without rebuilding the session;
- Opus system audio, remote clipboard, cursor shape, keyboard, and selectable absolute/relative mouse input;
- opt-in system OpenSSH discovery for a native SSH terminal and SFTP file transfer;
- Argon2id password verification and authenticated XChaCha20-Poly1305 traffic;
- diagnostics for capture, encode, transport, decode, rendering, and thread health.

The host defaults to 60 FPS, while a client requests up to 120 FPS by default when its display
supports it. The effective stream is the lower of the host limit, the client request, and the
active adaptive-quality limit. Both values are selectable in the welcome UI; during a session,
`F1` can change the client request live. Local presentation is configured separately as Auto,
60, 90, or 120 Hz. Auto uses the fastest advertised display cadence and enables 120 Hz
presentation on a supported MacBook Pro ProMotion panel.

## Platform support

| Platform | Host capture and audio | Client decode and render | Notes |
| --- | --- | --- | --- |
| macOS 14+ | ScreenCaptureKit video and system audio | VideoToolbox and Metal, with FFmpeg/SDL fallback | Requires Screen Recording and Accessibility permissions |
| Windows | Desktop Duplication/D3D11 and WASAPI loopback | FFmpeg or NVDEC; SDL/D3D11 rendering | NVENC works when exposed by the driver; the complete CUDA path is optional |
| Ubuntu | Persistent XShm capture and PulseAudio monitor source | FFmpeg and SDL3 | Host mode requires XFCE in an X11 session; Wayland is intentionally rejected |

The Windows capture watchdog treats `DXGI_ERROR_WAIT_TIMEOUT` as a normal static-desktop result,
not an automatic failure. If authenticated remote input proves that pixels should be changing,
a sustained no-frame stall first recreates only `IDXGIOutputDuplication` while preserving the
D3D11 device and CUDA-registerable textures. A continued or quick recurring stall escalates to
a full device rebuild. Hard DXGI texture/map failures still rebuild immediately; every recovery
purges stale paced video and sends one clean IDR.

The common protocol, transport, UI, and fallback media paths are C11. Native bridge units
are limited to Objective-C on macOS and optional CUDA C++ for the NVIDIA pipeline. There is
no Rust component.

## Quick start

1. Open GRD on the computer to be controlled.
2. Set a password of at least 12 characters; settings are saved automatically.
3. Choose a display, select the host FPS limit, and click **Join the LAN**.
4. On macOS, grant Screen Recording and Accessibility access, then relaunch the same app
   build if macOS requests it.
5. Open GRD on the client, select a discovered host, then provide its password and choose
   controller or observer mode in the connection dialog.
6. Choose the client stream FPS and display Hz in the connection card, then connect. Press `F1`
   to select Automatic, Absolute cursor, or Relative camera mouse mode; a click captures the
   mouse in Automatic/Relative mode. FPS and presentation Hz changes are applied live.

The host is advertised only after **Join the LAN** and disappears again when sharing stops.
TCP and media UDP use port `47990` by default; discovery uses UDP port `47989`.
If the host process is interrupted by a crash or an updater while sharing, the saved active
state restores the LAN host automatically on the next launch. An orderly **Leave the LAN** or
application shutdown clears that state.

## SSH terminal and SFTP

On macOS and Linux, GRD can advertise an existing system OpenSSH service next to the
remote-desktop action. This feature is deliberately opt-in and does not install, enable, or
reconfigure `sshd`.

1. Enable the operating-system SSH server yourself:
   - macOS: **System Settings > General > Sharing > Remote Login**;
   - Linux: install and start `openssh-server` using the distribution's service manager.
2. In GRD, open **Settings > Host**, enable **Publish SSH + SFTP on the LAN**, select the
   SSH port, and use **Verify service**.
3. Start the GRD host with **Join the LAN**. A client that receives the verified capability
   announcement displays **Terminal** and **File SFTP** beside that host.
4. Enter the remote operating-system account name. GRD opens the local system `ssh` or
   `sftp` client in a terminal.

GRD probes loopback for a valid `SSH-` banner before advertising these actions. OpenSSH—not
GRD—performs account authentication and host-key verification. SSH passwords and private
keys never enter GRD and are not stored in its configuration; only the last non-sensitive
username is remembered. Enabling discovery does not change the network exposure of the SSH
service itself, so restrict Remote Login/`sshd` users and firewall rules appropriately.

## Build from source

### Requirements

- CMake 3.25 or newer and Ninja;
- a C11 toolchain;
- macOS 14 or newer for a macOS build;
- SDL3;
- FFmpeg libraries: `libavcodec`, `libavutil`, `libswresample`, and `libswscale`;
- libsodium;
- Xcode Command Line Tools on macOS;
- X11, XRandR, XTest, and PulseAudio/PipeWire Pulse compatibility on Ubuntu;
- Windows SDK on Windows;
- optionally, the CUDA Toolkit for the complete NVIDIA conversion and interop path.

CMake downloads the pinned Nuklear `v4.13.3` source automatically.

### Development build

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

### Optimized build

```sh
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
```

Build outputs:

```text
macOS:          build/dev/grd.app
Windows:        build/dev/grd.exe
Linux:          build/dev/grd
Diagnostic CLI: build/dev/grd_diag
```

Disable CUDA explicitly with:

```sh
cmake --preset dev -DGRD_ENABLE_CUDA=OFF
```

On macOS, both build and install use `GRD_CODESIGN_IDENTITY`. Keeping the same bundle
identity and signing identity across rebuilds prevents macOS privacy permissions from being
associated with a different executable identity. A development certificate can be selected
at configure time:

```sh
cmake --preset dev \
  -DGRD_CODESIGN_IDENTITY="Apple Development: Your Name (TEAMID)"
cmake --build --preset dev
```

## Architecture deep dive

### Design rules

The implementation follows five rules:

1. **Capture must never wait for the network.** Encoded frames enter owned queues; only a
   per-client pacer calls `sendto()`.
2. **Input must not wait behind media.** Control and media use separate authenticated TCP
   connections, and video has its own UDP path.
3. **A damaged prediction chain must be repaired explicitly.** GRD drops dependent P-frames,
   requests an IDR, and resumes only from a valid reference.
4. **Latency wins over an ever-growing queue.** Queues are bounded and the newest complete
   usable frame is preferred at presentation time.
5. **Hardware acceleration is an optimization, not a protocol requirement.** Codec and
   backend negotiation always retain a software-compatible H.264 path.

### Source tree

```text
GRD
|-- src/app/             application entry point and Nuklear UI
|-- src/core/            config, discovery, protocol, transport, SSH/SFTP dispatch
|-- src/platform/        macOS, Windows, X11, input, clipboard, system OpenSSH
|-- src/video/           video/audio codecs, Metal, GPU selection, optional CUDA
|-- src/tools/           headless diagnostics
|-- tests/               auth, audio, codec, protocol, and transport tests
|-- packaging/           native installer/package definitions
|-- CMakeLists.txt       platform feature detection and build graph
`-- CMakePresets.json    reproducible development and release presets
```

### Runtime topology

The UI thread owns SDL event handling and rendering. Long-running capture, codec, and
transport operations run independently.

```text
                              HOST PROCESS

  +------------------+       +------------------+       +------------------+
  | SDL/UI main thread|       | grd-stream       |       | grd-audio        |
  | controls + status |       | capture / encode |       | capture / Opus   |
  +---------+--------+       +---------+--------+       +---------+--------+
            |                          |                          |
            |                          +------------+-------------+
            |                                       |
            v                                       v
  +------------------+       +---------------------------------------------+
  | grd-cursor       |       | Transport                                   |
  | shape + position |       | accept + UDP RX + per-connection TCP RX/TX |
  +------------------+       | + one priority pacer per logical client     |
                             +---------------------------------------------+


                             CLIENT PROCESS

  +------------------+       +------------------+       +------------------+
  | control TCP RX/TX|       | media TCP RX/TX  |       | media UDP RX     |
  +---------+--------+       +---------+--------+       | auth / FEC / NACK|
            |                          |                | reassembly        |
            +--------------------------+-------+        +---------+--------+
                                                   |              |
                                                   v              v
                                         +----------------+  +----------------+
                                         | audio decode   |  | video decode   |
                                         +-------+--------+  +-------+--------+
                                                 |                   |
                                                 v                   v
                                         +----------------------------------+
                                         | SDL/UI main thread: render/input |
                                         +----------------------------------+
```

The health monitor records the age of the stream, cursor, discovery, TCP RX/TX, UDP,
video-decode, and audio-decode workers. This makes a capture stall distinguishable from a
transport stall or a decoder/rendering stall.

### Session establishment

Discovery uses IPv4 multicast group `239.255.71.68` on UDP `47989`. Every app listens for
peers, but announces itself only while its host is accepting connections. A backward-safe
goodbye announcement removes a stopped host immediately instead of waiting for the normal
stale-peer timeout. A separate extended datagram carries the operating system and verified
SSH/SFTP capabilities while preserving the original discovery-packet layout for older GRD
builds. It also contains the base host identity, so discovery remains useful if Wi-Fi packet
reordering or multicast filtering drops one of the paired announcements.

Each logical client creates two TCP connections: one for control and one for media. After
authentication, the media connection establishes a UDP route using a random 16-byte token.

```text
CLIENT                                                     HOST
  |                                                          |
  |<--- active-host multicast announcement -------------------|
  |---- select host + authenticated TCP connect ------------->|
  |                                                          |
  |---- protocol v3 HELLO + ephemeral public key ------------>|
  |<--- nonce + server ephemeral public key ------------------|
  |---- HMAC transcript proof -------------------------------->|
  |<--- authentication result + encrypted session ------------|
  |                                                          |
  |==== authenticated control TCP ============================|
  |==== authenticated media TCP ==============================|
  |---- encrypted UDP probe carrying routing token ---------->|
  |<=== paced encrypted video/audio UDP ======================|
  |                                                          |
```

A host accepts one controller and up to three observers. Because every logical client has
two TCP halves, the transport can own up to eight authenticated TCP connections.

### Host video pipeline

```text
 display
    |
    v
+---------------------+     +---------------------+
| platform capture    | --> | fit / quality ladder|
| SCK / D3D11 / XShm  |     | cap + adaptive/user |
|                     |     | offload ladder      |
+---------------------+     +----------+----------+
                                   |
                                   v
                        +--------------------------+
                        | negotiated encoder       |
                        | H.264 / HEVC / AV1       |
                        +------------+-------------+
                                     |
                            owned ref-counted frame
                                     |
                                     v
                        +--------------------------+
                        | broadcast fan-out        |
                        +------------+-------------+
                                     |
                         +-----------+-----------+
                         |                       |
                         v                       v
                  +-------------+         +-------------+
                  | client pacer|   ...   | client pacer|
                  +------+------+         +------+------+
                         |                       |
                         +---- fragment / AEAD / FEC ----> LAN
```

The stream thread does not perform socket I/O and never sends while holding the host's
global mutex. A slow observer therefore cannot freeze capture for the controller.

### Selective client offload

GRD does not pretend that capture or encoding can be moved after the fact: the host must
still capture the desktop and produce the compressed bitstream. It can, however, encode
fewer pixels while the client performs presentation work on its own GPU. The client settings
keep each part independent instead of hiding the trade-off in one preset:

```text
HOST                                           CLIENT

D3D11 capture                                  VideoToolbox decode
     |                                                  |
     v                                                  v
NV12 conversion + NVENC  === encrypted LAN ===>  IOSurface / Metal
     ^                                                  |
     |                                                  +--> optional GPU upscale/filter
client-selected resolution rung                        +--> optional 120 Hz local pacing
                                                        `--> optional cursor prediction
```

| Client resolution work | Host encoding rung for a 1080p request | Intended trade-off |
| --- | ---: | --- |
| Native | 1920x1080 | maximum source detail |
| Balanced | 1600x900 | about 31% fewer encoded pixels; moderate client upscale |
| Maximum | 1280x720 | about 56% fewer encoded pixels; lowest host/network load |

Only the user's client-offload choice changes the encoded resolution rung. Transient congestion
never destroys and recreates the encoder behind a running game. Switching an explicit rung sends
a new video configuration and recovery IDR without reconnecting. Local pacing only repeats the
most recent decoded frame at the selected display cadence; it does not invent intermediate frames.

On Windows, a normal Desktop Duplication `WAIT_TIMEOUT` is treated as an unchanged desktop, not a
failure. The capture watchdog is armed only when the nominal 1 ms `AcquireNextFrame` call is
measured blocking inside the driver for a grossly longer interval. It renews the duplication
interface first and escalates a confirmed relapse to a rate-limited D3D11 rebuild. Consecutive
capture gaps form one recovery episode; after 150 ms of stable cadence GRD purges stale paced
frames once and sends one repair IDR. Five-second pipeline logs separate session/device resets and
report DXGI `AccumulatedFrames` as `coalesced`, exposing source-side capture pressure.

With the CUDA/NVENC pipeline, Windows capture remains GPU-resident: Desktop Duplication copies
into a persistent D3D11 BGRA texture, CUDA maps that texture and converts/scales directly into
the NV12 hardware frame consumed by NVENC. The texture unit performs bilinear sampling instead of
four manual surface reads per color sample, reducing competition with the game on the same GPU.
No staging `Map`, CPU readback, or host-to-device upload is performed. If D3D11/CUDA interop is
unavailable, GRD logs the reason once and safely falls back to the CPU capture path for that stream.

NVENC uses a low-latency quality configuration: `p3`, low-latency tuning, one-frame VBV,
no B-frames, no lookahead, and spatial/temporal AQ. This keeps the pipeline frame-bounded while
spending more of the encoder's work on perceptual detail than the former `p1` performance
configuration. The periodic `host stream` log includes a `zero-copy` frame count so the active
path can be verified from a real session.

### Client video pipeline

```text
 encrypted UDP
      |
      v
+---------------------+    missing fragment    +------------------+
| authenticate +      | ----------------------> | FEC, then NACK   |
| anti-replay window  |                         | if still missing |
+----------+----------+                         +------------------+
           |
           v
+---------------------+    four concurrent     +------------------+
| fragment reassembly | ----------------------> | complete frame   |
+---------------------+       frame slots       | bounded queue    |
                                                   +-------+------+
                                                           |
                                                           v
                                                   +--------------+
                                                   | decode thread|
                                                   +-------+------+
                                                           |
                                                    latest-frame mailbox
                                                           |
                                                           v
                                                   +--------------+
                                                   | Metal or SDL |
                                                   | aspect-correct|
                                                   +--------------+
```

The decoder receives only complete access units. Arrival gaps are evaluated against the
source timestamp cadence, so a legitimate 30 FPS source is not misclassified as network
jitter merely because frames arrive about 33 ms apart. A detected source-frame gap arms a
pre-decoder gate: dependent P-frames are discarded, an IDR is requested with bounded retries,
and decoding resumes only from the repaired reference chain. VideoToolbox `bad data` errors
recreate the decode session and follow the same recovery path rather than blindly continuing
a corrupted chain.

Rendering preserves the source aspect ratio using centered letterboxing or pillarboxing.
Metal uses immediate presentation matched to the selected display cadence, up to 120 Hz.
On a ProMotion Mac, Auto selects the panel's advertised maximum. Stream FPS remains separate:
it is negotiated from `target_fps` and `client_target_fps`.

### Codec and backend negotiation

Before streaming, a client advertises a decode capability mask for H.264, HEVC, and AV1. The
host selects the configured codec only if both an encoder and the client support it; otherwise
it falls back to H.264. The host welcome screen exposes all three choices; HEVC is the preferred
quality option for 1080p gaming when both endpoints support it. NVENC H.264 uses the High
profile for better coding efficiency without lookahead or additional buffered frames. The
effective codec and pipeline are sent in `VIDEO_CONFIG`.

| Path | Capture/input surface | Encode | Decode/render |
| --- | --- | --- | --- |
| Apple native | BGRA `CVPixelBuffer` from ScreenCaptureKit | VideoToolbox hardware session | VideoToolbox `CVPixelBuffer` to Metal texture |
| NVIDIA complete CUDA | Persistent pinned staging, double-buffered over two CUDA streams | CUDA RGBA-to-NV12 into NVENC buffers | NVDEC to CUDA/D3D11 texture interop |
| NVIDIA without CUDA runtime | Platform capture | NVENC with CPU color conversion | Available hardware or software decoder |
| Portable fallback | Platform capture and FFmpeg color conversion | FFmpeg software encoder | FFmpeg software decoder and SDL texture upload |

On macOS, the native decoder retains its `CVPixelBuffer` until Metal consumes the associated
texture. On a complete Windows CUDA path, the client avoids a device-to-host-to-device round
trip. These are zero-copy GPU paths; encryption and network packetization still require their
own bounded buffers.

### Buffer ownership

Encoded FFmpeg packets remain reference-counted from the encoder through broadcast. The UDP
fragmenter reads a small wire prefix and the encoded payload as separate parts instead of
joining them into another full-frame allocation. On the client, reassembly reserves decoder
padding and hands the completed allocation to FFmpeg with `av_buffer_create()`.

```text
 encoder AVBufferRef
        |
        +---- host broadcast reference ----+
                                           |
                                  per-client pacer reference
                                           |
                                      UDP fragments
                                           |
                                  client reassembly buffer
                                           |
                                  decoder adopts allocation
```

Ownership is explicit and reference-counted. Color conversion, authenticated encryption,
and individual UDP datagrams remain intentional copies.

## Transport and recovery

### Channel separation

| Channel | Carries | Reason |
| --- | --- | --- |
| Control TCP | keyboard, mouse, cursor state, ping, critical status | reliable and isolated from large media messages |
| Media TCP | video/audio configuration, clipboard, bitrate reports, recovery fallback | reliable without blocking input |
| Media UDP | video fragments, audio access units, NACK, FEC | low latency with application-controlled recovery |

Video is UDP-only. If the UDP socket or route handshake fails, GRD reports an error instead
of silently moving video to TCP and creating head-of-line latency. Audio normally fits in one
UDP datagram and temporarily falls back to media TCP during UDP recovery.

### Packet protection

TCP packets have a 20-byte framing header. UDP packets use a 36-byte header containing the
`GRDU` magic, protocol version, packet type, payload length, sequence, and 16-byte routing
token. Payloads are protected by XChaCha20-Poly1305, and the UDP header is authenticated as
associated data.

```text
UDP datagram (maximum 1200 bytes)

+----------------------+----------------------+------------------+
| 36-byte GRD header   | encrypted payload    | 16-byte AEAD tag |
+----------------------+----------------------+------------------+
                         ^
                         video payload includes a 20-byte
                         frame-fragment header when applicable
```

UDP sequences use a 1024-bit anti-replay/reordering window. A sequence is committed only
after authentication succeeds. With the current headers, a video fragment can carry up to
1128 bytes of encoded payload.

### Per-client pacer

Every client owns a bounded pacer queue with this strict priority order:

```text
highest                                                     lowest
 CONTROL -> AUDIO -> RETRANSMISSION -> FEC -> KEYFRAME -> VIDEO
```

The pacer has 32 queue entries and an eight-frame retransmission cache. Its pacing interval
is derived from the total wire-rate budget for a 1200-byte datagram and clamped between
100 and 2000 microseconds.

Admission control estimates whether a P-frame can finish before its deadline. If it cannot,
the complete frame is rejected before sending; GRD never emits only the first half of a
predicted frame. Keyframes receive a wider deadline of up to three frame periods.

### FEC, NACK, and reference recovery

Adaptive XOR FEC adds one parity fragment for each block of 16 data fragments, about 6%
overhead. The client reconstructs one missing fragment per block locally. If recovery is not
possible, it sends a compact authenticated UDP NACK, with a TCP fallback, within the recovery
window. Retransmissions re-enter the pacer at higher priority than new video.

Packet recovery and codec recovery solve different problems:

```text
 fragment missing
       |
       +--> XOR FEC succeeds ----------------------> complete frame
       |
       +--> NACK + cached retransmission succeeds -> complete frame
       |
       `--> frame deadline expires
                    |
                    v
             prediction chain invalid
                    |
        purge dependent P-frames + request IDR
                    |
                    v
           resume from clean reference frame
```

If a reference frame is dropped by the pacer, the host marks a discontinuity, purges or
rejects dependent P-frames, queues a recovery IDR, and suppresses pre-IDR encoder output.
The client's source-frame gap detector provides a second independent guard.

The pacer preserves up to 12 ms of previously unused wire time exclusively for a recovery
IDR. Ordinary P-frames remain strictly paced. This lets the existing encoder headroom absorb
a large scene-change keyframe instead of delaying its first dependent frames into another
drop/IDR cycle.

Normal NVENC operation uses a 10-second safety GOP instead of spending bitrate on an IDR every
two seconds. Recovery remains receiver-driven: startup, a capture reset, an unrepaired source
gap, or a pacer discontinuity forces an immediate IDR with SPS/PPS. Intra refresh stays disabled
because it cannot reliably bootstrap the VideoToolbox decoder after reference loss.

## Adaptive quality

The client reports authenticated UDP loss and RTT every 100 ms. The host keeps network loss
separate from its own one-second rolling pacer-drop ratio, because congestion on the sender
is not the same signal as packets lost on the LAN.

```text
                         +---------------------+
                         | STARTUP HOLD (3 s)  |
                         +----------+----------+
                                    |
                                    v
  loss or local drops      +--------+---------+      clean for 5 s
 +------------------------ | STEADY / PROBE   | -------------------+
 |                         +--------+---------+                    |
 |                                  |                              |
 |                                  | +200 kbps / 2 s              |
 |                                  +<-----------------------------+
 v
+---------------------+
| MULTIPLICATIVE CUT  |  thresholds: >=1%, >=3%, >=10%
+----------+----------+  network loss or local pacer drops
           |
           v
+---------------------+
| HOLD (8 s)          |  allow queues and encoder size to settle
+----------+----------+
           |
           `-------------------------------> STEADY / PROBE
```

Two consecutive lossy reports are required before a network cut, while host-local pacer
pressure must persist for three reports. Recovery is deliberately slow: after the hold, both
signals must remain clean for 50 reports (five seconds), and the target grows by 200 kbps every
two seconds. Native encoders receive a short grace period after a decrease. NVENC rate-control
fields are reconfigured live by FFmpeg on the next submitted frame; the encoder session stays
open and FFmpeg emits an atomic IDR for the new rate-control state. Small additive probes are
accumulated; upward encoder reconfiguration is applied at most once per 15 seconds so that an
IDR is not repeated for transient clean windows. An encoder without a verified live-rate path
keeps its current encoder and wire budget instead of being reopened during gameplay.

The configured rates are total on-wire budgets, not raw encoder payload rates. Encoder video
receives roughly 84% of the budget without FEC and 78% while FEC is active. Receiver UDP loss
controls the network ABR. Local admission pressure can lower both NVENC and the pacer only after
the live encoder update succeeds, avoiding a feedback loop in which an unchanged encoder is
forced through an increasingly smaller pacer.

### Dynamic frame-rate controller

The FPS selected by the user is a ceiling, not a promise to manufacture frames the host cannot
produce. GRD continuously combines three independent host-pressure signals:

- an EWMA of capture + conversion + encode + transport submission time;
- confirmed long DXGI capture gaps (using `AccumulatedFrames` only as evidence;
  values above one are normal when the display refreshes faster than the stream);
- the one-second initiating-drop ratio from pacer admission, deadline, queue, and send failures.

Pressure is scored with time-based decay. A single short hiccup therefore does not force a fixed
120 -> 90 transition. Sustained pressure moves the effective cadence in small even-numbered steps
towards the measured sustainable rate (for example 120 -> 116 -> 112), while preserving headroom.
After five clean seconds it climbs by 4 FPS every two seconds until it reaches the requested
ceiling. The encoder stays open throughout; only capture cadence, advertised stream FPS, and
pacer frame period change. The `host stream` diagnostic prints `fps effective/target`, the
pressure score, and the reason for the latest change. If capture and encode cleanly track a
lower compositor cadence without pipeline errors, the log reports `source-limited` instead of
blaming transport.

The client sends its live resolution cap to the host. Selecting 1440p or 4K in the welcome screen
or F1 overlay therefore changes the encoded dimensions without reconnecting; with the default
Native mode this is not client-side upscaling. Balanced and Maximum explicitly select one or two
lower encoding rungs and let the client's GPU scale them. Auto uses the client display size,
capped at 3840x2160. A rolling 128-sample QP history remains available for diagnostics, but no
longer causes an automatic encoder reopen or resolution switch.

For demanding games, the host UI provides an adaptive 20-30 Mbps preset. It stores
`initial=20000`, `target=30000`, and `min=14000`, enables ABR, and selects the gaming profile.
On a clean LAN, parity is disabled and those reserved bits improve picture quality; sustained
packet loss enables FEC, cuts bitrate quickly, and retains protection through a clean recovery
window before the additive ramp resumes. Local pacer pressure also lowers the encoder budget
without enabling FEC, preventing rare reference-frame purges from recurring at an otherwise
clean maximum bitrate.

## Input, audio, and session controls

### Input

Keyboard, buttons, absolute pointer motion, and scroll use the control TCP channel. Relative
motion uses the authenticated low-latency UDP channel with automatic TCP fallback. Only
consecutive absolute motion events are coalesced; button and keyboard ordering is preserved.
Wire keyboard codes use USB HID/SDL scancodes, and GRD converts `Command` and `Ctrl` when
crossing between macOS and Windows/Linux. Local cursor prediction hides one network round trip
for absolute cursor movement.

During a remote session:

- the remote desktop starts in full-screen mode by default;
- `F1` is the only local session shortcut and opens the settings overlay;
- while a remote session is active, macOS close requests are ignored so `Cmd+W` reaches the
  controlled computer as `Ctrl+W`; disconnect from the `F1` overlay before closing GRD locally;
- the overlay requests 30, 60, 90, or 120 stream FPS from the host without reconnecting;
- the overlay negotiates Auto, 1080p, 1440p, or 4K encoded resolution without reconnecting;
- local presentation can use Auto/ProMotion or a 60, 90, or 120 Hz cap independently of the
  stream frame rate;
- Native, Balanced, and Maximum client-work modes decide whether the host encodes the requested
  resolution or one/two lower ladder rungs;
- local display pacing, GPU video filtering, and absolute-cursor prediction are separate
  switches, saved and applied immediately;
- an optional non-interactive gaming-style top-right HUD shows decoded FPS and frametime history,
  target FPS, measured/encoder bitrate, network health, the active hardware pipeline,
  encoded resolution, presentation cadence, source skips, IDR recoveries and decoder errors;
- the video quality filter uses GPU linear sampling plus Retina pixel-grid alignment; disabling
  it selects the cheaper nearest sampler rather than applying a pixel-art filter to game video;
- the mouse selector offers Automatic click-to-capture, Absolute cursor for cursor-driven
  applications, and Relative camera for first- or third-person games;
- the overlay can toggle full-screen and set mouse sensitivity as a numeric multiplier from
  `0.25` to `3.00`, where `1.00` is one-to-one movement;
- keys such as `Esc` and `Alt+Tab` are forwarded to the remote computer;
- pressing `Esc` three times within two seconds releases the captured mouse, after forwarding
  the key events;
- clicking the remote image captures relative mouse input again unless Absolute mode is selected;
- Windows `SendInput` rejections are returned to the client log/status and counted in the HUD,
  making an integrity/UIPI mismatch visible instead of looking like mouse lag.

### Audio

System audio is encoded as Opus at 48 kHz stereo, 128 kbps, in 10 ms access units (480
samples). The audio queue is bounded to six frames and has higher pacer priority than video.
On Linux, GRD uses Pulse source `@DEFAULT_MONITOR@`; set `GRD_AUDIO_SOURCE` to another monitor
source name when required.

### Clipboard and cursor

UTF-8 text and clipboard data use media TCP so a large clipboard transfer cannot sit in front
of input. Cursor position travels on the control path. X11 cursor shape and hotspot are sent
to the client when available.

## Configuration and diagnostics

GRD stores `config.ini` and `grd.log` in the platform application-data directory:

```text
macOS:   ~/Library/Application Support/GRD/
Windows: %APPDATA%\GRD\
Linux:   ~/.config/grd/
```

Important defaults:

| Key | Default | Meaning |
| --- | ---: | --- |
| `target_fps` | `60` | maximum captured/encoded stream rate |
| `client_target_fps` | `120` | client stream-rate request, capped by the host and display |
| `presentation_hz` | `0` | local presentation cap; `0` selects Auto/ProMotion |
| `client_max_height` | `1440` | requested encoded height; `0` is Auto, then `1080`, `1440`, or `2160` |
| `client_upscale_mode` | `0` | `0` Native, `1` Balanced (one lower rung), `2` Maximum (two lower rungs) |
| `client_frame_pacing` | `1` | present locally at the selected display cadence; `0` caps presentation to stream FPS |
| `client_cursor_prediction` | `1` | predict absolute cursor motion locally and reconcile it with host state |
| `show_advanced_stats` | `1` | show the persistent live top-right diagnostics HUD during a session |
| `sharp_video_scaling` | `1` | use the GPU linear quality filter; `0` selects nearest sampling |
| `mouse_mode` | `0` | `0` Automatic, `1` Absolute cursor, `2` Relative camera |
| `initial_bitrate_kbps` | `20000` | session startup wire-rate budget |
| `target_bitrate_kbps` | `24000` | hard ABR ceiling |
| `min_bitrate_kbps` | `10000` | ABR floor |
| `abr_enabled` | `1` | enable adaptive bitrate |
| `stream_profile` | `0` | `0` balanced, `1` gaming, `2` desktop |
| `video_codec` | `0` | `0` H.264, `1` HEVC, `2` AV1 |
| `pixel_444` | `0` | prefer 4:4:4 when the selected path supports it |
| `mouse_sensitivity_percent` | `100` | persisted compatibility value; shown as `1.00` in the UI |
| `remote_fullscreen` | `1` | start the remote session in full-screen mode |
| `ssh_remote_access_enabled` | `0` | advertise a verified system OpenSSH service on macOS/Linux |
| `ssh_remote_access_port` | `22` | system SSH/SFTP port verified before capability discovery |
| `remote_access_username` | empty | last non-sensitive operating-system username used by the client |

Saved configuration takes precedence over new defaults. For example, an older
`target_bitrate_kbps=30000` remains in effect until it is edited or replaced.

### Logs

Every five seconds, `grd.log` records correlated host and client telemetry:

- `host stream`: capture rate, encoder output, QP, ABR state, and active pipeline;
- `host tx`: paced frames/fragments, self-drops, retransmissions, FEC, and audio datagrams;
- `client rx`: datagram rate, sequence loss, arrival jitter, incomplete frames, and NACKs;
- `client dec`: decoder rate, queue drops, cadence-aware arrival/source gaps, repair-IDR
  requests, and presentation behavior;
- thread and pipeline maps: last-progress ages and selected capture/codec/render backends.

Use timestamps to align `host tx` with `client rx`, then `host stream` with `client dec`:

```text
capture gap?       host stream frame cadence stops first
sender congestion? host tx self-drop rises before client loss
LAN loss?          client rx loss/NACK rises without host self-drop
decode stall?      client rx stays healthy while client dec cadence stops
render stall?      decode stays healthy while present cadence becomes irregular
```

`grd_diag` is a headless real-stream diagnostic client useful for transport and decoder tests
without the graphical application. On a Windows CUDA build,
`grd_diag --cuda-selftest` launches a small conversion kernel and detects an executable built
for an incompatible GPU architecture. It does not require an active capture session.
`grd_diag --capture-selftest 10 120` performs a local 120 FPS capture/encode test and succeeds only
when all captured frames travel through the native D3D11 -> CUDA -> NVENC path. It reports
average/maximum time for the complete pipeline and its acquire, conversion, encoder-submit, and
packet-receive stages; it does not start a host, open a port, or read/change the configured
password.

## Optional SFTP and PowerShell access

The Windows host can be provisioned with an isolated OpenSSH service for key-only SFTP and
optional PowerShell-over-SSH. This service is disabled until an administrator explicitly
applies a per-user policy. It uses TCP `47992` by default, a Private/LocalSubnet firewall
rule, dedicated standard Windows accounts, and credentials completely separate from GRD.
Privileged PowerShell accounts remain rejected unless the administrator supplies the explicit
high-risk `-AllowPrivilegedPowerShellAccounts` override; SFTP never accepts that override.

The available profiles are `SftpRead`, `SftpWrite`, and `PowerShell`. Controller and observer
roles receive none of them automatically. See [REMOTE_ACCESS.md](REMOTE_ACCESS.md) for the
permission matrix, setup, audit, client commands, revocation procedure, and security limits.
One Windows account can authorize up to 16 ED25519 client keys while the legacy single
`PublicKey` policy field remains supported.

## Windows CUDA build

The standard Windows installer does not require the CUDA Toolkit. GRD can use NVENC whenever
the NVIDIA driver exposes `h264_nvenc`; without the CUDA runtime, color conversion falls back
to the CPU while NVENC remains active.

The complete CUDA path -- GPU-resident D3D11-to-NV12 conversion, NVENC/NVDEC, and D3D11
interop -- has specific toolchain requirements:

- `nvcc` accepts Visual Studio `cl.exe` as its Windows host compiler, not MinGW GCC or GNU-mode
  Clang;
- GRD's C99 compound literals require `clang-cl` for C translation units;
- MSYS2 pkg-config include directories can shadow UCRT/MSVC headers, so the build uses a small
  dependency include shim containing only SDL3, FFmpeg, and libsodium headers;
- MSYS2 static FFmpeg archives pull VAAPI/QSV/X11 dependencies into an MSVC link, so the
  recipe uses the `lib*.dll.a` import libraries;
- `src/platform/windows.c` supplies classic COM/D3D11 GUID definitions omitted by Windows SDK
  10.0.26100 for this compiler combination.

CUDA verification builds use `-DGRD_REQUIRE_CUDA=ON`. Unlike the normal optional
`GRD_ENABLE_CUDA` switch, this mode fails configuration if `nvcc` is unavailable instead of
silently compiling `cuda_stub.c`; both CI and the Windows updater therefore prove that the real
`cuda_pipeline.cu` and CUDA/D3D11 interop sources were selected.

### `update-grd-cuda.bat`

Run `update-grd-cuda.bat` from the repository root on the Windows host. It performs:

```text
git pull
  -> detect the local NVIDIA compute capability
  -> configure Release with CUDA
  -> build
  -> run unit tests
  -> execute a real CUDA conversion kernel
  -> stage grd.exe and runtime DLLs in dist\windows-nvenc
  -> launch GRD
```

If GRD is already open, the script closes it before replacing the executable. Configure the
variables near the top of the script:

```bat
set "MINGW=C:\msys64\ucrt64"
set "MSYS=C:\msys64\usr"
set "VSDIR=C:\BuildTools\VS2022"
set "CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
set "LLVM=C:\Users\<user>\llvm22\clang+llvm-22.1.8-x86_64-pc-windows-msvc"
set "DEP_INC=C:\Users\<user>\grd-headers"
set "BUILD_DIR=build\release-cuda"
set "DIST_DIR=dist\windows-nvenc"
```

Prerequisites:

- MSYS2 packages
  `mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,sdl3,ffmpeg,libsodium}`;
- Visual Studio Build Tools with the **Desktop development with C++** workload;
- LLVM for Windows with `clang-cl`;
- CUDA Toolkit with `nvcc`;
- an NVIDIA display driver that provides `nvidia-smi`.

The updater reads GPU 0's compute capability through `nvidia-smi` and configures the matching
CMake architecture automatically (`75` for the GTX 1660, for example). A stale or incompatible
`CUDA_ARCH` environment value is replaced by the detected value. Before publishing, the updater
executes a real CUDA kernel; if detection or kernel execution fails, it automatically rebuilds
with CUDA conversion disabled so the host can continue through CPU conversion instead of sending
no video. The previous executable in `dist` is not replaced unless the selected build and its unit
tests succeed. The script creates `DEP_INC` automatically if missing. Update its hard-coded Windows
SDK `rc.exe` path if the installed SDK is not `10.0.26100.0`.

## Packaging

The native packaging workflow builds and verifies:

- `GRD-0.2.9-Windows-x64-Setup.exe` with Inno Setup, including silent install/uninstall tests;
- `grd_0.2.9_amd64.deb` on Ubuntu 24.04, inspected with `dpkg-deb`;
- the macOS application bundle.

The Windows installer adds firewall rules for TCP `47990`, media UDP `47990`, and discovery
UDP `47989` on private network profiles only, then removes those rules on uninstall.

## Security model and scope

GRD stores a libsodium Argon2id password verifier and random salt, never the plaintext
password. Authentication uses ephemeral X25519 keys and an HMAC-SHA256 transcript proof over
the nonce and both public keys. The session key is derived from the shared secret, password
verifier, and nonce. Directional XChaCha20-Poly1305 keys protect traffic with strictly
monotonic sequences.

This design provides authenticated encryption for a trusted LAN, but it is **not a formally
verified PAKE** such as OPAQUE. A recorded handshake permits offline guesses against a weak
password, and the project has not received an independent security audit.

Do not expose GRD's ports to the Internet. Internet support would require, at minimum, a
reviewed PAKE implementation, durable replay protection, rate limiting, signed updates, and
an audited QUIC/TLS transport.

## Project status

GRD is an actively developed, LAN-only remote desktop. Hardware paths and low-latency recovery
are implemented, but compatibility still depends on drivers, capture permissions, codecs,
and the host's ability to encode at the requested frame rate. Logs and reproducible test cases
are especially valuable when reporting a stall, quality regression, or backend fallback.
