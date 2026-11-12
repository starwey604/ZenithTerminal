# ZenithTerminal

面向 Radxa ZERO 3E 的 RoboMaster 2027 自定义终端。
当前支持本机/RK3566 双平台构建、DRM 能力探测，以及本机 VKMS 双 plane
合成与 writeback 桌面预览。原有线程、protobuf、SDL3、Paho smoke 继续保留。

```bash
git submodule update --init --recursive
cmake --preset native
cmake --build --preset native
ctest --preset native
```

- [SDK 接入与双平台构建](doc/SDK_INTEGRATION.md)
- [Radxa SDK 源码准备及固定版本](doc/SDK_SETUP.md)
- [开发计划：VKMS 双 plane 桌面预览](doc/DEVELOPMENT_PLAN.md)

程序位于 `build/native/bin/ZenithTerminal`。运行 `--self-check` 无需窗口、
broker 或目标板。目标工具链准备完成后，使用 `rk3566-clang` preset；
SDK 路径通过 `-DZT_RK_SDK_ROOT=...` 指定。

## DRM 能力探测

```bash
cmake --preset native -DZT_ENABLE_DRM=ON
cmake --build --preset native
./build/native/bin/zenith_drm_probe --driver vkms
```

依赖、交叉构建与运行结果见 [DRM 探测指南](doc/DRM_PROBE.md)。
模块边界与实施顺序见 [模块划分](doc/MODULES.md)。

## VKMS 双 plane 预览

```bash
cmake --preset native -DZT_ENABLE_DRM=ON -DZT_ENABLE_PREVIEW=ON
cmake --build --preset native
# 先按指南释放 VKMS，再运行：
./build/native/bin/ZenithTerminal --drm-demo --take-over --frames 0
```

[运行、设备交接与恢复指南](doc/VKMS_PREVIEW.md) 包含 TEST_ONLY、截图、
交叉编译和实测结果。默认启动与 `--self-check` 仍只运行原有 smoke。

RM2027 的 1080p 调试使用 `--size 1920x1080 --pipeline --uncapped`；
需要先临时加载下面记录的累计 VKMS 补丁。

[分段计时、对照实验与 VKMS 优化补丁](doc/VKMS_PERFORMANCE.md)。
