# Radxa ZERO 3E SDK 准备

SDK 源码已放在 `/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4`，已接入
Radxa 板级配置和内核，并选中 ZERO 3E。匹配 rootfs 的基础目标工具链与
sysroot 已在 Ubuntu 22.04 容器中构建完成；应用用法见
[SDK 接入与双平台构建](SDK_INTEGRATION.md)。

## 当前配置

准备日期：2026-11-12。

| 项目 | 内容 |
| --- | --- |
| 原始包 | `/home/ww/下载/rk356x_linux6.1_rkr4_sdk.repo.tar`，保留原文件 |
| SDK 根目录 | `/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4` |
| 原厂基线 | `RK3566_RK3568_LINUX6.1_SDK_RELEASE_V1.0.1_20240920` |
| 源码项目数 | 52 |
| 板级配置 | `rockchip_rk3566_radxa_zero_3e_defconfig` |
| 设备树 | `rk3566-radxa-zero-3e.dts` |
| 内核 | Radxa 6.1.84；`kernel` 链接到 `kernel-6.1` |
| Buildroot 配置 | `rockchip_rk3566_defconfig` |
| 目标架构 | AArch64 / Cortex-A55 |
| rootfs 工具链配置 | Buildroot 内部 GCC 12.4.0、glibc 2.38 系列、C++ 支持 |
| 已选择的显示与解码包 | libdrm（含 atomic）、Rockchip MPP（DRM allocator） |

固定的源码版本：

- `device/rockchip`：`d064c6c075f3b93ef41ed999993fd8d01eed9cfa`，来自
  `radxa/device-rockchip` 的 `rk3566_rk3568-linux-6.1` 分支。
- `kernel-6.1`：`af558fa36b0a7fcd293ec7a8e8912b7fc8c25aef`，来自
  `radxa/kernel` 的 `linux-6.1-stan-rkr4.1-buildroot` 分支。
- `buildroot`：`6982a513fe9405d133d62528061a99b386d68c53`。
- `external/mpp`：`eff1e2ede31817c7d6fbe9c62421ab4955f962b7`，保留原 SDK 配套版本。

SDK 内另附 Arm GCC 10.3 预编译工具链，但当前 rootfs 配置使用 Buildroot
内部工具链。应用不得仅因为前者已经可执行就把它的 libc/sysroot 当作当前
rootfs 的开发环境。后续 Clang 应使用 Buildroot 实际生成的目标支持文件。

## 配置和版本记录位置

以下路径相对于 SDK 根目录：

- `.repo/local_manifests/radxa-zero3e.xml`：生效的 Radxa 覆盖清单，提交已固定。
- `output/.config`：已生成的 ZERO 3E SDK 配置。
- `output/defconfig`：指向 ZERO 3E 板级配置。
- `buildroot/output/rockchip_rk3566/.config`：已生成的 Buildroot 配置。
- `.setup/preparation.json`：本机准备记录。
- `.setup/zero3e-config.log`、`.setup/buildroot-config.log`：配置日志。
- `.setup/rockchip-original.xml`：切换前的原厂提交清单。
- `.setup/radxa-zero3e-pinned.xml`：接入 Radxa 后的完整提交清单。

本仓库的 `sdk/radxa-zero3e/` 保存对应覆盖清单、原厂清单和最终清单，供重建
与版本审查使用。完整清单中的原厂相对 remote 依赖原始 SDK 的 manifest
仓库地址，不是一个可脱离原 SDK 直接 `repo init` 的独立发行包。

## 在新的空目录中重建源码环境

下面命令只用于新解压的、没有自行修改的 SDK。`--force-sync` 用于首次从
原厂项目名迁移至 Radxa 项目名；不要对已有开发修改的 SDK 盲目重复执行。

```bash
SDK=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4
ZT=/home/ww/codings/ZenithTerminal
mkdir -p "$SDK"
tar --extract --no-same-owner \
  --file=/home/ww/下载/rk356x_linux6.1_rkr4_sdk.repo.tar \
  --directory="$SDK"
cd "$SDK"
.repo/repo/repo sync -l -j4
mkdir -p .repo/local_manifests
cp "$ZT/sdk/radxa-zero3e/local-manifest.xml" \
  .repo/local_manifests/radxa-zero3e.xml
.repo/repo/repo sync --force-sync --no-manifest-update \
  -c --no-tags --no-clone-bundle -j2 device/rockchip kernel-6.1
./build.sh rockchip_rk3566_radxa_zero_3e_defconfig
make -C buildroot O="$SDK/buildroot/output/rockchip_rk3566" \
  rockchip_rk3566_defconfig
```

覆盖清单保留了 SDK 原有 `build.sh`、`Makefile`、`rkflash.sh` 和 `kernel`
链接入口，不覆盖其他厂商组件。两个 Radxa 项目采用浅历史获取；原始提交
记录和原始下载包保留，后续需要历史时再获取。

## 源码与工具链状态

已完成源码解包、52 个项目的本地检出、Radxa 覆盖、ZERO 3E 和 Buildroot
配置。最终清单中所有项目提交均与实际检出版本一致，板级配置和设备树匹配。

已在 `zt-sdk-ubuntu2204` 中完成 `make ... BR2_JLEVEL=3 toolchain`，生成
GCC 12.4、glibc 2.38、libstdc++ 和 Radxa 内核用户态头文件。日志位于
SDK 的 `.setup/toolchain-build.log`。

应用的 `ZT_RK_SDK_ROOT` 指向
`buildroot/output/rockchip_rk3566/host`，不是 BSP 根目录。此 `host` 目录
可原地用于应用开发，尚未打包成可搬移 SDK。已在同一容器中构建 SDK 的
libdrm 2.4.120（沿用原补丁及依赖），并安装到 sysroot 和 target；日志位于
`.setup/libdrm-build.log`，应用接入见 [DRM 探测指南](DRM_PROBE.md)。
截至 2026-11-12，SDK 的内核、固件及 MPP 目标库尚未编译；当时只构建应用需要的
工具链与 libdrm，没有构建包含 Chromium/Weston 的默认 rootfs。

## ZERO 3E 第一版开发固件（2026-11-16）

已新增 ZenithTerminal 专用板级/Buildroot 配置，完成 U-Boot、原版 Radxa 6.1.84
内核及 DTB、精简 ext4 rootfs、MPP/RGA、recovery 和 `update.img` 构建。
主 rootfs 位于 `buildroot/output/rockchip_rk3566_zenith/`，原工具链输出保留。

SDK 的所有本轮源码修改由本仓库 `patches/radxa-zero3e/` 管理，使用
`tools/zero3e-sdk` 检查/应用/导出，`tools/zero3e-build all` 执行完整构建。
板端默认 `192.168.77.3/24`，root 空密码，Dropbear 允许免密码、免公钥登录；
仅用于隔离直连开发网络。

[开发固件指南](ZERO3E_BRINGUP.md) 包含构建、镜像检查、串口与 SSH 使用方法。
当前已通过实际镜像检查及 ARM SSH 服务的 QEMU 免认证登录验证。
[Linux SD 制卡指南](ZERO3E_SD_CARD.md) 提供 raw 整盘镜像生成与确认后写卡入口，
实体写卡和实板启动仍需确认目标设备后执行。

VKMS 双 plane/writeback 桌面预览计划见 `DEVELOPMENT_PLAN.md`。
