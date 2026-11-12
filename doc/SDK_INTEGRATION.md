# 构建本机与 RK3566 示例

同一份 `main.cpp` 验证线程与时钟、protobuf 序列化往返（含 proto3 optional）、
SDL3 ARGB 离屏绘图，以及无需连接 broker 的 Paho MQTT C/C++ 客户端创建。
本机和 RK3566 使用独立的依赖产物；本机不需要 RK SDK 或 DRM 设备。

## 本机运行

宿主机需要 Clang、LLD、LLVM 静态库工具、CMake ≥ 3.24、Ninja、Git，以及
C/C++ 标准库开发文件。应用构建在宿主机运行，Ubuntu 容器仅用于构建 BSP
工具链。依赖源码通过 Git 子模块固定，不使用发行版的 protobuf/Paho/SDL。

```bash
cd /home/ww/codings/ZenithTerminal
git submodule update --init --recursive
cmake --preset native
cmake --build --preset native
ctest --preset native
./build/native/bin/ZenithTerminal --self-check
```

成功时输出 `SELF-CHECK PASS`。程序不打开窗口、不连接 broker、不访问 DRM。

## 在 Ubuntu 22.04 中构建匹配的 SDK 工具链

完整 SDK 源码准备方式见 [SDK 源码准备](SDK_SETUP.md)。当前已选定
`rockchip_rk3566_radxa_zero_3e_defconfig` 和 Buildroot 的
`rockchip_rk3566_defconfig`，目标为 AArch64/Cortex-A55，使用 GCC 12.4、
glibc 2.38 系列及 Radxa 6.1 内核头文件。

容器名为 `zt-sdk-ubuntu2204`。首次创建：

```bash
distrobox create --yes --name zt-sdk-ubuntu2204 \
  --image docker.io/library/ubuntu:22.04 \
  --home /home/ww/codings/sdks/.distrobox/zt-sdk-ubuntu2204 \
  --volume /home/ww/codings:/home/ww/codings:rw --no-entry \
  --additional-packages 'build-essential git wget curl ca-certificates bc bison flex cpio rsync unzip file patch python3 python3-dev python3-setuptools libncurses-dev libssl-dev gawk texinfo help2man autoconf automake libtool pkg-config expect patchelf cmake ninja-build'
distrobox enter --name zt-sdk-ubuntu2204
```

在容器中构建，后续失败后恢复也使用同一条 `make ... toolchain`：

```bash
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
unset LD_LIBRARY_PATH
SDK=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4
cd "$SDK"
make -C buildroot O="$SDK/buildroot/output/rockchip_rk3566" \
  BR2_JLEVEL=3 toolchain
```

此目标只构建工具链及其必要组件，不构建 Chromium、Weston 或整套固件。
不更换默认 rootfs 的 libc，也不使用包内另附 GCC 10.3 的 sysroot。
初次从宿主机配置切换到 Ubuntu 时，已将宿主机生成的 Kconfig 工具移至
`.setup/buildroot-config-before-ubuntu`，并在容器中重新生成，避免混用宿主机
glibc 版本。之后保持在同一容器中构建这一套 Buildroot 输出。

## 交叉编译应用

`ZT_RK_SDK_ROOT` 是 **Buildroot 的 `host` 目录**，或者具有相同布局、已经
完成重定位的导出应用 SDK 根目录。它必须包含
`aarch64-buildroot-linux-gnu/sysroot/usr/include/stdio.h`。不要传 BSP 源码根目录
或仅有运行文件的 `target` 目录。

```bash
cd /home/ww/codings/ZenithTerminal
SDK_HOST=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4/buildroot/output/rockchip_rk3566/host
cmake --preset rk3566-clang -DZT_RK_SDK_ROOT="$SDK_HOST"
cmake --build --preset rk3566-clang
python3 tools/check-rk-elf.py build/rk3566-clang/bin/ZenithTerminal \
  --sdk-root "$SDK_HOST"
```

默认 preset 指向 `$HOME/codings/sdks` 下当前 SDK 的 `host`，可用上述参数覆盖。
切换为不同 ABI 的 SDK 时使用新的构建目录，避免复用旧依赖。

Clang/LLD 在宿主机运行，使用 SDK 的启动文件、内核用户态头文件、glibc、
libgcc 和 libstdc++；CPU 参数为 `-march=armv8-a -mtune=cortex-a55`。
不使用 `-march=native`，也不为这一步重建 libc++。

检查脚本验证 AArch64 ELF、目标加载器、SDK 依赖库和逐库符号版本要求，
并拒绝 RPATH/RUNPATH。交叉程序不注册为宿主机 CTest；板到货后再执行
`ZenithTerminal --self-check`。ELF 检查不替代真实板端运行。

## 在本机模拟运行 ARM 产物

Ubuntu 容器已安装 `qemu-user`。在新容器中可用
`sudo apt-get install --no-install-recommends qemu-user` 安装，然后从宿主机执行：

```bash
SDK_HOST=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4/buildroot/output/rockchip_rk3566/host
distrobox enter --name zt-sdk-ubuntu2204 --no-tty -- \
  qemu-aarch64 -L "$SDK_HOST/aarch64-buildroot-linux-gnu/sysroot" \
  /home/ww/codings/ZenithTerminal/build/rk3566-clang/bin/ZenithTerminal --self-check
```

这验证 AArch64 程序与目标运行库的用户态配合，不模拟 MPP、DRM 或实际板端性能。

## 构建目录和依赖边界

每个 preset 在 `build/<preset>/` 下维护以下目录：

- `host/`：从固定 protobuf 源码构建的本机 protoc。
- `deps/`：各依赖的独立构建目录。
- `staging/`：对应目标架构的 protobuf、Paho C/C++、SDL3 安装前缀。
- `app/`：独立应用 CMake 构建，含 `compile_commands.json`。
- `bin/ZenithTerminal`：示例程序。
- `install/bin/ZenithTerminal`：安装产物。

顶层 superbuild 依次完成 host protoc、目标 protobuf、Paho C、Paho C++、SDL3
和应用配置。host protoc 与目标 protobuf 固定同一版本，生成代码时显式使用
host protoc。应用 `find_package()` 指定 staging 中的 CMake 配置，不回退到
宿主机安装的同名库。交叉构建中的 pkg-config 仅查询 SDK sysroot；应用依赖
使用 CMake 导出，避免把 staging 的绝对路径再次加上 sysroot。

默认每个依赖使用 2 个编译任务，可在 configure 时设置 `-DZT_BUILD_JOBS=3`。
依赖按顺序构建，避免多个子构建同时放大并发。

首个示例静态链接应用依赖，libc 和 C++ 系统运行库仍动态链接。Paho TLS
关闭；默认 SDL 只启用离屏所需能力。可选 `ZT_ENABLE_PREVIEW=ON` 为本机开启
X11/XWayland 窗口，所有构建均关闭 SDL KMSDRM。BUAA 子模块保持原版本，
尚未链接；其拆分与 VAAPI/MPP 在后续里程碑接入。
各依赖版本见 [第三方依赖](../3rdparty/README.md)，后续路线见
[开发计划](DEVELOPMENT_PLAN.md)。

## 2026 年 11 月 12 日验证结果

- Ubuntu 22.04.5 / GCC 11.4 成功构建 SDK 的 GCC 12.4 与 glibc 2.38 工具链。
- 宿主机 Clang 23.1.4 / CMake 4.4.5 成功构建两个 preset。
- 本机 x86-64 程序通过 CTest 和全部四项自检。
- 交叉程序为 AArch64，解释器为 `/lib/ld-linux-aarch64.so.1`，依赖与符号版本
  均由 SDK 提供，无 RPATH/RUNPATH；交叉构建使用的 protoc 为 x86-64 程序。
- QEMU 6.2 加载 SDK sysroot 后，AArch64 程序全部四项自检通过。
- 错误 SDK 路径在 configure 阶段失败；ELF 检查脚本会拒绝本机 x86-64 程序。

日志：`build/native/test.log`、`build/rk3566-clang/elf-check.log`、
`build/rk3566-clang/qemu-test.log`。两套构建日志分别在对应目录的 `build.log`。
目标板尚未到货，实板运行待验证；MPP 和业务 UI 仍按后续计划推进。

## 可选 DRM 探测模块

`ZT_ENABLE_DRM=ON` 构建 `zenith_display_drm` 与 `zenith_drm_probe`。
交叉编译前还需在 SDK 中构建 libdrm，步骤及运行结果见 [DRM 探测指南](DRM_PROBE.md)。
不启用该选项时，原有 smoke 仍不需要 libdrm。

本机双 plane/writeback 示例、桌面控制权交接和相同显示代码的交叉构建见
[VKMS 预览指南](VKMS_PREVIEW.md)。
