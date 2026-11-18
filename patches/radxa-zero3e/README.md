# ZERO 3E SDK 补丁

在 ZenithTerminal 中维护 SDK 修改，在外部 SDK 中构建。先执行检查和应用：

```bash
cd /home/ww/codings/ZenithTerminal
SDK=/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4
./tools/zero3e-sdk check "$SDK"
./tools/zero3e-sdk apply "$SDK"
./tools/zero3e-sdk status "$SDK"
```

`series.tsv` 固定每个 SDK 子仓库的基线提交和补丁目录；目录内按文件名排序。
工具先检查所有子仓库，再应用补丁。补丁保留为 SDK 工作区修改，不创建提交、
不切分支、不改真实 Git index。重复应用完整补丁集会跳过；基线不符、补丁路径
有其他修改、只应用了一部分补丁或存在同名未跟踪文件时会停止。
其他路径的本地修改不被覆盖，也不自动收进补丁。

已经应用第一版（`device/rockchip` 只有 0001–0003）的 SDK，在确认板级文件没有
自行修改后，可单独补上 0004，再正常应用整套补丁。干净 SDK 不需要这一步：

```bash
patch="$PWD/patches/radxa-zero3e/device-rockchip/0004-select-zero3e-slim-kernel-fragment.patch"
git -C "$SDK/device/rockchip" apply --check "$patch" &&
git -C "$SDK/device/rockchip" apply "$patch" &&
./tools/zero3e-sdk apply "$SDK"
```

## 当前补丁

- `buildroot/0001-*`：新增 `rockchip_rk3566_zenith_defconfig`、静态网络与
  Dropbear 配置，以及仅对该 defconfig 生效的后处理；原厂 rootfs 和 recovery
  配置不变。
- `device-rockchip/0001-*`：新增 ZERO 3E Zenith 板级配置，选择上述 rootfs，
  固化主机名，关闭 Wi-Fi/蓝牙、USB gadget、输入事件及 async-commit 用户空间服务。
- `device-rockchip/0002-*`：修复构建 hook 的失败返回码和 init 阶段错误传播。
  原代码在 `if ! command; then` 中读取 `$?`，实际读到的是取反后的 0。
- `u-boot/0001-*`：允许通过 `RK_BUILD_JOBS` 限定并发；未设置时保留原行为。
- `device-rockchip/0003-*`：让内核、主 rootfs 和 recovery 构建使用相同的
  `RK_BUILD_JOBS` 并发限制；未设置时保留厂商默认行为。
- `device-rockchip/0004-*`：只为 ZERO 3E Zenith 板级配置设置 `RK_KERNEL_CFG_FRAGMENTS`。
- `kernel-6.1/0001-*`：新增这个配置片段，关闭 Wi-Fi、蓝牙、CAN、TV/DVB、广播/SDR 和
  SATA/NVMe；保留 HDMI、USB 主机/键鼠、MPP 和 recovery 所需的 USB 功能。

厂商 defconfig、设备树和内核源码未改动。Radxa 源码覆盖清单继续由
`sdk/radxa-zero3e/local-manifest.xml` 管理，内核补丁基线与固定源码清单一致。

## 将后续 SDK 修改带回仓库

在 SDK 中修改并验证后，用 `export` 导出**明确指定的路径**。它从固定基线生成
完整差异，包含所选目录内的新文件，不改 SDK 的真实 index，也不覆盖已有输出文件：

```bash
./tools/zero3e-sdk export "$SDK" --project buildroot \
  --path configs/rockchip_rk3566_zenith_defconfig \
  --path board/rockchip/zenith-zero3e \
  --path board/rockchip/common/overlays/90-zenith-zero3e \
  --output /tmp/zero3e-buildroot-replacement.patch
```

这是该子仓库相对于基线的**替换补丁**，不要直接作为第二个增量补丁叠加。
审查后替换该目录的旧补丁集，再运行 `check`；现有 SDK 若正好等于新补丁的最终
状态会识别为 `applied`。如果要分多个增量补丁，按修改顺序从独立开发分支导出
`git format-patch`，在基线 SDK 上验证完整序列后更新目录。

不要把 SDK 的输出目录、下载缓存、个人 SSH 密钥或整套 SDK 源码加入本仓库。
恢复原厂版本时先保存自己的修改，再对**具体补丁和路径**执行逆向应用；工具不提供
整树 reset/clean。补丁集变更后，不用旧补丁逆向恢复新 SDK。

## 检查补丁工具

```bash
python3 -m unittest discover -s tests -p test_zero3e_sdk.py
```

覆盖有依赖的连续补丁、重复应用识别、部分应用、同名新文件冲突、暂存区冲突、
基线不符、其他路径修改保留及新文件导出。
