# 构建 ZERO 3E 开发固件

开发固件使用 Radxa 6.1 内核和 ZERO 3E 专用裁剪片段，保留 HDMI、USB 键鼠、
MPP 和 recovery。精简 Buildroot 提供串口恢复 shell 和 `192.168.77.3/24` 上的 SSH。

## 一条入口构建完整固件

在已按 [SDK 准备](SDK_SETUP.md) 检出的固定版本 SDK 上执行：

```bash
cd /home/ww/codings/ZenithTerminal
./tools/zero3e-build all
```

入口先检查并应用本仓库补丁，再在 `zt-sdk-ubuntu2204` 容器中选择专用板级配置，
调用原有 `build.sh all` 完成 loader、内核、rootfs、recovery、分区镜像和
`update.img` 打包。失败返回非零，日志保存在
`build/zero3e-firmware/logs/`。完整步骤不调用 `cleanall`。

默认 SDK 为 `/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4`。
可用 `ZT_SDK_ROOT`、`ZT_SDK_CONTAINER`、`ZT_SDK_JOBS` 覆盖路径、容器和并发，
默认并发为 6，通过 `RK_BUILD_JOBS` 统一传递给 U-Boot、内核和 Buildroot。
若缓存齐全但 SDK 的下载镜像连通性探测失败，可用
`RK_NETWORK_CHECK= ./tools/zero3e-build all`，在 SDK 提示时按回车。
该设置仅让预检查失败时继续，实际下载失败仍会中止构建。

首次整套构建还需要补齐容器依赖（在宿主机执行）：

```bash
distrobox enter --name zt-sdk-ubuntu2204 --no-tty -- \
  sudo apt-get install -y --no-install-recommends \
  python2 python-is-python3 device-tree-compiler u-boot-tools \
  lz4 libgmp-dev libmpc-dev expect-dev
```

Python 2 仅供旧版 U-Boot 脚本使用，`python` 指向 Python 3，不按旧指南全局指向
Python 2。SDK 的生成工具也必须在该 Ubuntu 容器内编译。此次迁移已将之前由宿主机
生成、不能在 Ubuntu glibc 上运行的 `output/kconf` 移到
`.setup/kconf-before-ubuntu2204`，由容器重新生成；后续不要混用宿主机编译它。

## 专用配置和输出互不混用

| 项目 | 本轮配置或路径（相对 SDK） |
| --- | --- |
| 板级配置 | `rockchip_rk3566_radxa_zero_3e_zenith_defconfig` |
| Buildroot | `rockchip_rk3566_zenith_defconfig` |
| Buildroot 输出 | `buildroot/output/rockchip_rk3566_zenith/` |
| 内核 | `rockchip_linux_defconfig` + `rk3566_zenith_zero3e_slim.config`；主系统和 recovery 共用 |
| 设备树 | `rk3566-radxa-zero-3e.dts` |
| 启动格式 | 厂商 FIT，保留 recovery |
| 固件目录 | `output/firmware/`，SDK 的 `rockdev/` 链接到此处 |

原来的 `buildroot/output/rockchip_rk3566/host` 继续保留，既有应用 preset 不变。
新系统开发优先显式指定新固件的 `buildroot/output/rockchip_rk3566_zenith/host`，
避免以后两套配置演进后误用 sysroot。

主系统保留 glibc/C++、BusyBox init、eudev、Dropbear、iproute2、ethtool、iperf3、
strace、procps、lrzsz、libdrm/modetest、MPP 测试与 RGA。去掉 Chromium/Weston、
Qt、摄像头/GStreamer、音频、无线用户空间和 USB gadget 服务，只生成 ext4 主 rootfs。
不启动会修改 DRM 状态的 async-commit 服务，给应用保留直接 KMS 控制权。
镜像不预装或自启动 ZenithTerminal；需要时在启动后通过 SSH 部署。

```bash
# 只生成配置；或修复失败原因后重跑对应阶段
./tools/zero3e-build configure
./tools/zero3e-build kernel
./tools/zero3e-build rootfs
./tools/zero3e-build recovery
./tools/zero3e-build firmware
```

rootfs/recovery 阶段需要已构建的 loader/内核。修改 rootfs 后还要运行 `firmware`
重新打包；只重建 rootfs 不会更新旧的 `update.img`。首次验证完成后，如要再次检查
整套入口，仍使用 `all`。更换 libc、工具链或删除已构建的软件包时使用新的输出目录，
不要把简单增量构建当作删除包后的干净镜像。

## 核对实际镜像

```bash
SDK=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4
python3 tools/check-zero3e-firmware.py --sdk-root "$SDK" \
  --report build/zero3e-firmware/report.json
```

检查工具使用 `debugfs` 只读查看 ext4 中的网络、主机名、串口 shell、Dropbear 服务和
实际 AArch64 程序，同时检查 FIT/Rockchip 包头、必需镜像及裁剪后的 Buildroot 配置。
宿主机没有 `debugfs` 时，可在同一 Ubuntu 容器中运行该命令。

**`update.img` 是 Rockchip 升级包，不是可直接 `dd` 的 SD 整盘镜像。**
Linux 下可继续按 [SD 制卡指南](ZERO3E_SD_CARD.md) 生成 raw 整盘镜像。
实际写卡前先确认目标设备；板端启动需要串口验收。

## 上板后直接通过 SSH 登录

- 板卡：`192.168.77.3/24`；主机名：`zenith-zero3e`。
- 电脑有线口：`192.168.77.1/24`；不为该连接设置默认网关和 DNS。
- 板端仅 `eth0` 使用静态配置，不启动有线 DHCP 客户端。
- 调试 UART2：物理 Pin 6 GND、Pin 8 TX、Pin 10 RX；3.3V TTL，
  1500000/8N1/无流控，TX/RX 交叉，不接串口适配器 VCC。

按开发需求，root 密码为空，Dropbear 使用 `-B` 接受空密码及 SSH `none` 认证，
不要求密码或客户端公钥。电脑配置好有线地址后直接执行：

```bash
ssh root@192.168.77.3
```

首次连接仍可能要求确认板卡主机指纹，这是客户端识别服务器，不是登录密码或公钥
授权。Dropbear 自动生成主机密钥并保存到可写 ext4 的 `/etc/dropbear/`，普通重启后
保留，重刷 rootfs 后会变化。镜像不内置共用的 SSH 主机私钥。

**任何能连接 SSH 端口的人都能获得 root 权限。** 此配置仅用于隔离的直连开发网络，
不要把该镜像接入公共或不受信任的网络。串口仍保留厂商的直接 root 恢复 shell。

每版固件上板后，先验串口、SD 启动和直连 SSH，再部署程序运行
`ZenithTerminal --self-check`。显示用 DRM 探测和实际出图验收，MPP 用短片实际解码；
键鼠通过 USB Hub 插拔并检查输入事件，recovery 检查启动和维护介质识别。

补丁维护、应用、导出及冲突处理见 [SDK 补丁说明](../patches/radxa-zero3e/README.md)。

## 2026-11-16 构建结果

已构建 loader、U-Boot、原版 6.1.84 内核/DTB、ext4 rootfs、recovery、OEM、userdata
及完整 `update.img`。主 rootfs 约 47.8 MiB，完整升级包约 161.0 MiB。

- 实际 ext4 内容检查通过：静态 IP、主机名、root 空密码、Dropbear `-B`、串口 shell、
  AArch64 程序，以及无桌面/USB gadget/async-commit 启动服务。
- 从 boot FIT 提取的 DTB 为 `radxa,zero-3e`，GMAC1 启用，内核控制台为 `ttyFIQ0`。
- 从镜像提取 ARM Dropbear，在 QEMU 和独立临时根目录中测试；客户端关闭密码、
  公钥和交互认证后，以 `none` 方式登录并成功执行命令。
- 补丁工具与 overlay 回归测试通过；固定基线的干净检出可应用补丁并识别重复应用。

构建与镜像记录：`doc/evidence/zero3e-firmware-2026-11-16.json`。
SSH 与补丁验证记录：`doc/evidence/zero3e-ssh-none-2026-11-16.json`。
Linux 整盘镜像生成和离线检查已完成，见 [SD 制卡指南](ZERO3E_SD_CARD.md)。

## 2026-11-18 实板结果

首版完成实体 SD 卡烧录、读回校验及两轮串口启动。从隔离直连关闭密码、公钥及交互认证后，
OpenSSH 确认以 `none` 登录 `root@192.168.77.3`；板端返回 `uid=0`、主机名
`zenith-zero3e` 和内核 `6.1.84`。三次冷启动和网线拔插恢复验收尚未完成。

应用使用 `rockchip_rk3566_zenith/host` 交叉编译，并通过 SSH 安装到板端
`/usr/bin/ZenithTerminal`；没有重启或重刷。`ZenithTerminal --self-check` 在 RK3566
实板退出码为 `0`，线程/时钟、protobuf、SDL3 离屏绘制及 Paho MQTT 离线客户端均通过。
应用未设为开机自启动，本次自检不接管显示，也不测试 MPP。

## 2026-11-18 内核裁剪版

关闭 Wi-Fi、蓝牙、CAN、TV/DVB、广播/SDR 和 SATA/NVMe。保留 HDMI（含音频）、
USB Host/HID/evdev、MPP/RGA、SD/eMMC、串口和有线网。主系统仍不启动 USB gadget
服务；共用内核保留 gadget/FunctionFS 和 USB 存储，供 recovery 的 ADB 和 U 盘使用。

| 产物 | 首版（MiB） | 裁剪版（MiB） | 减少（MiB） |
| --- | ---: | ---: | ---: |
| 内核 `Image` | 38.26 | 35.05 | 3.21 |
| `boot.img` | 38.61 | 35.40 | 3.21 |
| `recovery.img` | 47.45 | 44.24 | 3.21 |
| `rootfs.img` | 47.76 | 47.15 | 0.61 |
| `update.img` | 161.02 | 154.00 | 7.02 |
| raw SD（本机磁盘占用） | 160.44 | 153.42 | 7.02 |

Buildroot 配置未改动，`rootfs.img` 是重新生成的 ext4 镜像。raw SD 的逻辑大小仍为
7 GiB，分区边界和 rootfs UUID 不变。

产物位于 `build/zero3e-kernel-slim/`：`firmware/update.img` 是完整 Rockchip 升级包，
`zenith-zero3e-slim-sd.img` 是可写卡的整盘镜像，`comparison.json` 记录精确字节数。
从实际 FIT 提取核对了内核配置；boot 和 recovery 使用相同内核，各自 DTB 与首版逐字节
一致，recovery 的文件清单不变。整盘镜像的 GPT 和源内容对照通过。裁剪版尚未上板，
HDMI 实际出图、USB 键鼠、MPP 解码和 recovery 启动留待备用 SD 卡验收。
