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
| BUAABackend | existing project pin | d9f72e5db23d8e6be5f3ef6986c3c283ca8927bb |

BUAABackend is reserved for the protocol/video integration milestone and is not
linked by the platform smoke test. Its FFmpeg-coupled receiver must be split before
the MPP backend is integrated. Source license files remain in each submodule.

The smoke profile builds static application dependencies, disables Paho TLS, and
builds SDL for offscreen surface rendering without X11, Wayland, KMSDRM or GPU
backends. These are explicit smoke-test settings, not the final UI profile.
