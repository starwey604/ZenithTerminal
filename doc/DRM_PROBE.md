# 查询 DRM 与 VKMS 能力

`zenith_drm_probe` 输出设备、connector/encoder/CRTC 路由、plane 格式及 modifier、
属性范围/枚举，以及 writeback 格式。库 API 位于
`src/display/drm/include/zenith/display/probe.hpp`；返回自持有的快照，供后续显示模块复用。

## 构建和运行

现有双平台 smoke 不要求 libdrm。新功能通过 `ZT_ENABLE_DRM=ON` 启用；
本机需要 pkg-config 和 libdrm 开发文件（至少 2.4.110；CachyOS 的 libdrm 包包含头文件）。

```bash
cmake --preset native -DZT_ENABLE_DRM=ON
cmake --build --preset native
ctest --preset native
./build/native/bin/zenith_drm_probe --driver vkms
```

`--driver vkms` 自动定位卡号，不假设 `card0` 或 `card1`。也可以指定
`--device /dev/dri/cardN`；两个选择器互斥。不带参数查询所有 card 节点。
只读打开设备，不申请 DRM master，不分配 FB，不进行 modeset、atomic 提交或
TEST_ONLY；只对自己的 fd 设置 atomic/universal planes/writeback 查询可见性。
使用 `drmModeGetConnectorCurrent`，不强制触发显示器重新探测。

如果 VKMS 尚未加载，由用户在终端运行：

```bash
sudo modprobe vkms enable_overlay=1 enable_writeback=1
```

程序本身不加载模块、不自动提权、不卸载已在使用的模块。若模块已加载，上述
命令不会修改既有参数。可读取 `/sys/module/vkms/parameters/` 确认实际配置。
模块加载可能让桌面环境自动启用虚拟输出；实际提交测试前应先安排显示控制权，
不能直接从桌面抢占设备。

返回码：`0` 查询完成；`1` 打开/查询失败（包括部分卡查询失败）；`2` 参数错误；
`3` 没有匹配设备。查询成功不表示硬件满足双 plane 合成要求；看实际能力与候选。
权限失败保留具体路径与错误，不把它伪装成“不支持”。

## 为 RK3566 准备 libdrm

目标 libdrm 必须来自所选 SDK。沿用 SDK 的版本、补丁和 Buildroot 配置，
不要把本机 libdrm 复制进 sysroot，也不单独下载另一版板端 libdrm。
在之前建立的 Ubuntu 22.04 容器中执行：

```bash
distrobox enter --name zt-sdk-ubuntu2204
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
unset LD_LIBRARY_PATH
SDK=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4
make -C "$SDK/buildroot" O="$SDK/buildroot/output/rockchip_rk3566" \
  BR2_JLEVEL=3 libdrm
exit
```

该目标会构建 libdrm 所需的 SDK 依赖，并安装到 sysroot 与 target；不构建整套固件。
随后在宿主机构建应用：

```bash
cmake --preset rk3566-clang -DZT_ENABLE_DRM=ON
cmake --build --preset rk3566-clang
SDK_HOST=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4/buildroot/output/rockchip_rk3566/host
python3 tools/check-rk-elf.py build/rk3566-clang/bin/zenith_drm_probe --sdk-root "$SDK_HOST"
```

本机通过 pkg-config 找系统 libdrm；交叉工具链把 pkg-config 查询限制在 SDK
sysroot，仍使用本机 Clang/LLD。探测程序只链接 display 库与 libdrm，不链接
protobuf/Paho/SDL。外层 superbuild 保留原有顺序，也继续构建已有 smoke。
构建产物是 `build/<preset>/bin/zenith_drm_probe`，安装副本在 `install/bin/`。
需要恢复仅 smoke 构建时，重新配置 `-DZT_ENABLE_DRM=OFF`。

## 如何读报告

- `possible_crtcs` 的每一位对应报告中的 **CRTC index**，不是 CRTC 对象 ID。
- `AR24` 是 DRM 的 ARGB8888；NV12 的图像内存 plane 与硬件显示 plane 是两回事。
- `IN_FORMATS` 按 format/modifier 配对解码；缺少该属性时标记为未知，不假定 LINEAR。
- 属性同时输出当前值、可写范围或枚举及 immutable 标志；未暴露的 zpos/alpha/
  pixel blend mode 明确标记为 `not exposed`，不等同于没有像素透明混合。
- 末尾 NV12 + AR24 候选只检查不同非 cursor plane 的格式与共同 CRTC 路由。
  层序、透明混合、buffer 布局与缩放仍需真实缓冲的 atomic TEST_ONLY 和合成验证。
- 当前快照是顺序查询，设备热拔插或并发配置改变时可能失败；可重新运行探测。

## 本机 VKMS 实测（2026-11-12）

内核 `7.3.3-1-cachyos`；本机 libdrm `2.4.134`。用户加载模块后，
`--driver vkms` 查询 `/dev/dri/card0` 成功（卡号与对象 ID 不作为程序常量）。

- atomic、universal planes、writeback 查询可见性均成功。
- dumb buffer、PRIME import/export、ADDFB2 modifier capability 均可用。
- 1 个 CRTC：ID 86；1 个 primary、8 个 overlay、1 个 cursor。
- 所有这些 plane 均列出线性 NV12 和 ARGB8888；初始候选为 primary 36 + overlay 41。
- writeback connector 88，格式为 AR24、XR24、AB24、XR48、AR48、RG16。
- 存在 plane IN_FENCE_FD、CRTC OUT_FENCE_PTR、WRITEBACK_OUT_FENCE_PTR。
- 存在 YUV COLOR_ENCODING/COLOR_RANGE；没有 zpos、全局 alpha、pixel blend mode 属性。
- 查询时 CRTC ACTIVE=1、primary 已绑定 FB；后续真实提交前须处理桌面/设备所有权。

probe 仍只执行查询。另一个应用模式已完成 atomic TEST_ONLY、双 plane 合成与
writeback 桌面预览，见 [VKMS 预览指南](VKMS_PREVIEW.md)。
完整原始报告保存在 `doc/evidence/vkms-probe-2026-11-12.txt`。

## 检查

无设备 CTest 覆盖格式/modifier blob 的 offset、bit63、越界/截断、writeback 格式解析、
CRTC index 路由、cursor 排除、属性范围、未知 modifier、CLI 错误返回码。
默认测试不打开任何真实 DRM 卡；VKMS 集成查询用上面的显式命令运行。

参考：[内核 KMS 属性与 writeback](https://docs.kernel.org/gpu/drm-kms.html)、
[VKMS 配置](https://docs.kernel.org/gpu/vkms.html)。

### 双平台验收结果

- 本机原有 smoke、DRM blob/报告单元测试、CLI 帮助及错误路径均通过 CTest。
- SDK 的 libdrm 2.4.120 构建成功；应用用本机 Clang 23.1.4 链接 SDK sysroot
  中的 libdrm，未混入本机 2.4.134。
- `zenith_drm_probe` 交叉产物为 AArch64，加载器 `/lib/ld-linux-aarch64.so.1`；
  ELF 检查通过，无 RPATH/RUNPATH，12 项符号版本要求由 SDK 满足。
- QEMU 加载同一 sysroot 后，ARM probe 的 `--help` 与 ARM `zenith_tests` 均通过。
  QEMU 只验证用户态加载与无设备逻辑；RK 实机探测留待设备到货。

日志：`build/native/drm-tests.log`、`build/native/app/Testing/Temporary/LastTest.log`、
`build/rk3566-clang/drm-build.log`、`drm-elf-check.log`、`drm-qemu-help.log`、
`drm-qemu-unit.log`（后三项也在 `build/rk3566-clang/`）。
