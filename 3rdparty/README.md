# Pinned application dependencies

Initialize the source checkouts with `git submodule update --init --recursive`.
The superbuild uses these sources for both native and AArch64 libraries, and builds
a separate native protoc from the same protobuf revision. No source download occurs
during CMake configure/build.

| Source | Version | Commit |
| --- | --- | --- |
| protobuf | v21.12 / 3.21.12 | f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c |
| paho-mqtt-c | v1.3.15 | 62782406bcf511ee8e22bc34e826f8c6b8254c7c |
| paho-mqtt-cpp | v1.5.3 | 17ff3dc0270738adc710667be44847eebc038ae0 |
| SDL | release-3.2.30 | f5e5f6588921eed3d7d048ce43d9eb1ff0da0ffc |
| BUAABackend | starwey604 fork / zenith/ffmpeg-compat | 6f67fcd9db7a5ec08b5b787597faf358ba031b7c |

The video transport target compiles BUAABackend's pinned `udp_header.cpp` directly.
Zenith's AU reassembly adapter is independent of its BGR/FFmpeg receiver; the
public BGR APIs remain unchanged. The fork fixes FFmpeg key-frame metadata access
without adding its software decoder to Zenith. MQTT/client API integration is pending.
The original platform smoke does not require the video transport target.
Source license files remain in each submodule; the adapter retains a BUAA MIT notice.

Native `ZT_VIDEO_BACKEND=vaapi` additionally uses host pkg-config FFmpeg
(libavcodec/libavformat >=60, libavutil >=58) for file demux and VAAPI integration.
This explicit desktop dependency is not bundled in `3rdparty` or used in RK builds;
the SDK ABI and the pinned application dependencies above remain unchanged.

The smoke profile builds static application dependencies, disables Paho TLS, and
builds SDL for offscreen surface rendering without X11, Wayland, KMSDRM or GPU
backends. These are explicit smoke-test settings, not the final UI profile.

## BUAA fork and compatibility tests

The submodule now uses [starwey604's fork](https://github.com/starwey604/RM2027-CustomTerminal-CommunicationBackend),
based on upstream `d9f72e5db23d8e6be5f3ef6986c3c283ca8927bb`. The maintained change
is on `zenith/ffmpeg-compat`; the parent gitlink pins the exact commit above, not a
moving branch. `origin` is the fork; `upstream` in the existing checkout retains
BUAA's original repository. Fresh clones only need the fork to retrieve the pin.
For an existing checkout, run `git submodule sync -- 3rdparty/BUAABackend` before
`git submodule update --init --recursive` after pulling the parent update.

Both `decode()` and `flush()` use `AV_FRAME_FLAG_KEY` when the installed headers
provide it, otherwise the legacy `key_frame` field. HEVC I/P/B frame counts,
BGR pixels, key flags and delayed flush output passed on FFmpeg 9.0.2 and Ubuntu
22.04's FFmpeg 4.4.2. This compatibility range belongs to BUAA's original software
codec; Zenith's VAAPI backend still requires its newer FFmpeg dependencies above.

To build all original BUAA targets and the opt-in regression using Zenith's
already-built native protobuf/Paho dependencies, run from the Zenith root:

```bash
cmake -S 3rdparty/BUAABackend -B build/buaa-ffmpeg9 -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/native/staging" \
  -DProtobuf_PROTOC_EXECUTABLE="$PWD/build/native/host/bin/protoc" \
  -DRM_BUILD_VIDEO_TESTS=ON
cmake --build build/buaa-ffmpeg9
ctest --test-dir build/buaa-ffmpeg9 --output-on-failure
```

The regression requires FFmpeg's `libx265` encoder; no GPU or DRM master is needed.
On 2026-11-12 the original UDP sender/receiver also decoded all 91 transmitted
frames with zero errors/incomplete frames; four saved frames passed the upstream
pixel comparison (mean absolute errors 1.71–1.75, threshold 12).
