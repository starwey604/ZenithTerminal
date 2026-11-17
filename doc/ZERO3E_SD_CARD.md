# Linux 下生成和写入 ZERO 3E SD 卡镜像

先构建 SDK 固件，再生成一份可以逐字节写入 SD 卡的整盘镜像：

```bash
cd /home/ww/codings/ZenithTerminal
./tools/zero3e-build all
./tools/zero3e-sd build
```

SDK 固件没有变化时，只运行第二条即可。生成镜像不需要 root，不挂载文件系统，
不使用 loop 设备，也不读取或写入实体盘。

输出文件：

- `build/zero3e-sd/zenith-zero3e-sd.img`：**实际写卡用的 raw 整盘镜像**。
- `build/zero3e-sd/zenith-zero3e-sd.idbloader.img`：从本次 SDK loader 转换的 SD 启动镜像。
- `build/zero3e-sd/zenith-zero3e-sd.json`：分区、源文件、偏移和检查结果。

默认整盘镜像逻辑长度 **7 GiB，即 7,516,192,768 字节**，适用于实际容量不少于这个
值且逻辑扇区为 512 字节的卡。通常可使用标称 8 GB 或更大的卡，写卡脚本仍以设备
报告的实际容量判断。镜像是稀疏文件，当前实际占用约 161 MiB；复制到不保留稀疏性的
位置时可能占满 7 GiB。

SDK 路径默认沿用现有环境，也可显式指定：

```bash
./tools/zero3e-sd build \
  --sdk-root /home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4 \
  --output build/zero3e-sd/zenith-zero3e-sd.img \
  --size-mib 7168
```

再次构建会原子替换同名生成镜像。保留其他版本时使用不同的 `--output`。
生成器拒绝块设备、符号链接和会覆盖 SDK 固件输入的输出路径。

## 准备 Linux 工具

需要 Python 3.9+、`sfdisk` 和 `lsblk`，以及已构建 SDK 自带的
`rkbin/tools/boot_merger`。镜像生成与写卡均在 Linux 宿主机运行，不依赖 Windows
SDDiskTool 或 Ubuntu 构建容器。

Ubuntu/Debian 可安装：

```bash
sudo apt-get install python3 fdisk util-linux
```

Arch 系发行版对应 `python` 和 `util-linux`。`sfdisk` 需支持
`--relocate gpt-bak-std` 和 `--lock`；已在本机 util-linux 2.42.4 上验证。

## 沿用厂商启动链与分区布局

`update.img` 是 Rockchip 升级包，`MiniLoaderAll.bin` 是 USB 下载 loader 包。
**两者都不能当作 raw 整盘镜像直接写卡。**

生成器使用 SDK 自带工具，从本次构建的 loader 中转换 DDR/SPL 为 RK35 新 IDB：

```text
boot_merger idb -l -n -o <idbloader.img> <MiniLoaderAll.bin>
```

随后使用 `sfdisk` 生成主/备 GPT，写入 IDB 和各分区的原始内容；不解压/重打包 FIT，
不换 bootloader、不新增 extlinux 配置，也不修改 SDK 源码。

| 内容 | 起始位置 | 分区大小 |
| --- | --- | --- |
| 主 GPT | LBA 1，分区表从 LBA 2 开始 | 标准 GPT |
| RKNS IDB（DDR + SPL） | LBA 64，32 KiB | 当前约 312 KiB |
| uboot | LBA 16384，8 MiB | 4 MiB |
| misc | 12 MiB | 4 MiB |
| boot（内核/DTB FIT） | 16 MiB | 64 MiB |
| recovery | 80 MiB | 128 MiB |
| backup | 208 MiB | 32 MiB，保留空白 |
| rootfs | 240 MiB | 6144 MiB |
| oem | 6384 MiB | 128 MiB |
| userdata | 6512 MiB | 到备 GPT 之前 |
| 备 GPT | 镜像末尾 | 标准 GPT |

分区值取自 SDK 的 `output/firmware/parameter.txt`。rootfs 的 GPT UUID 保持为
`614e0000-0000-4b53-8000-1d28000054a9`，与现有内核的 `root=PARTUUID=614e0000-0000`
对应。rootfs 内容逐字节复制，因此继续使用 `192.168.77.3/24` 和 root 免密码、
免公钥 SSH；仅在隔离的直连开发网络使用。

依据：SDK 的 `rkbin/README` 区分 USB loader 与 IDB，`u-boot/tools/rkcommon.c`
定义 RK35 新 IDB，`u-boot/cmd/rkimgtest.c` 对 RKNS 直接识别为启动介质。这里不套用
旧 IDB 的“修改第 616 字节”做法，以免破坏新头部的 BootROM 完整性字段。
[U-Boot 官方 Rockchip 文档](https://docs.u-boot.org/en/latest/board/rockchip/rockchip.html)
也说明了 SD 启动区的 LBA 64/16384 布局；本方案使用当前 SDK 的 loader 和参数，
而不是替换为主线 U-Boot。

## 离线检查镜像

```bash
./tools/zero3e-sd check build/zero3e-sd/zenith-zero3e-sd.img
sfdisk --verify build/zero3e-sd/zenith-zero3e-sd.img
```

生成时自动检查：

- 主/备 GPT 及其格式要求的 CRC、保护 MBR、分区边界和 rootfs UUID。
- LBA 64 的 RKNS IDB，以及 BootROM 要求的头部和 DDR/SPL SHA256 字段。
- U-Boot、boot、recovery 的 FIT 位置与长度，以及 ext4 超级块位置。
- IDB 和所有写入分区的内容与源文件逐字节一致。

`check` 可独立检查 raw 镜像的结构，不需要 SDK；源文件逐字节比较在 `build` 阶段完成。
实际写卡后，通过串口验收实体板的 BootROM、DDR、U-Boot 和内核启动。

## 确认目标设备后再写卡

**当前仅生成镜像，没有写入任何实体盘。** 执行写卡前，需要确认目标整盘设备的路径、
型号、实际容量和序列号（设备能提供时），并接受该设备原有分区数据丢失。

先插入读卡器，查看设备，再对你确认的候选设备执行只读检查：

```bash
lsblk -o NAME,PATH,SIZE,MODEL,SERIAL,TRAN,RM,RO,MOUNTPOINTS
# 以下 /dev/sdX 是占位符，必须替换为确认过的整盘设备，不能填写分区节点。
./tools/zero3e-sd plan build/zero3e-sd/zenith-zero3e-sd.img /dev/sdX
```

`plan` 不写盘；会拒绝分区、loop/mapper、只读盘、容量不足、4K 逻辑扇区，以及设备
自身或子设备上有挂载点、swap、活动堆叠设备的情况。不自动卸载文件系统或接管磁盘。
如需卸载，应先确认属于目标 SD 卡，再由操作人卸载对应分区，重新运行 `plan`。

**只有目标设备已明确确认后**，才执行：

```bash
sudo /home/ww/codings/ZenithTerminal/tools/zero3e-sd write \
  /home/ww/codings/ZenithTerminal/build/zero3e-sd/zenith-zero3e-sd.img /dev/sdX
```

脚本再次展示设备信息，要求在交互终端中完整输入 `ERASE /dev/sdX`；路径是脚本显示
的实际整盘路径。输入不符、非交互运行或确认期间设备身份/容量/挂载状态变化，均停止。

确认后依次执行：

1. 独占打开目标盘，写入镜像的全部逻辑字节，包括稀疏区域对应的零。不做实体盘稀疏跳写。
2. 刷新写入数据并清除该设备的块缓存，将整个镜像范围从卡上读回，逐字节比较。
3. 用 `sfdisk` 将备 GPT 移到实际卡尾，只扩大最后的 userdata 分区，保留其他分区边界和 UUID。
4. 检查调整后的主/备 GPT 和启动区。userdata 文件系统由 SDK 的首次挂载流程扩容。

脚本在写入、读回和两步 GPT 调整期间持续持有独占设备声明及磁盘锁，并禁止 `sfdisk`
重新读取分区表，避免桌面在流程中自动挂载新分区。完成后安全移除
读卡器；如需在电脑上查看新分区，重新插入。写入或读回失败时不要启动这张卡，保存错误
输出并重新检查目标设备；写入开始后，原有数据已可能被覆盖。

## 上板验收

1. 板卡断电，插入 SD 卡，连接 3.3V TTL 串口，1500000/8N1/无流控。
2. 先打开串口并记录日志，再上电，依次确认 DDR、U-Boot、内核和 root shell。
3. 电脑有线口设为 `192.168.77.1/24`，连接网线，执行 `ssh root@192.168.77.3`。
4. 连续三次冷启动和一次网线拔插均恢复正常后，再开展应用自检和内核裁剪。

接线和登录细节见 [开发固件指南](ZERO3E_BRINGUP.md)。

## 已完成的离线验证

当前已生成 7 GiB raw 镜像，源 payload 比较和原生 `sfdisk --verify` 均通过。
另用普通镜像文件验证了 7 GiB → 8 GiB 的卡容量调整，不使用实体盘或 loop 设备。

```bash
python3 -m unittest discover -s tests -p test_zero3e_sd.py
```

测试覆盖参数解析、边界/UUID、GPT/IDB 损坏、较大卡的 GPT/分区调整、完整写入与读回的
普通文件模拟，以及挂载盘/错误设备/缺失确认的拒绝路径。普通文件传输模拟仅替换设备
身份、ioctl 和交互输入，实际复制、读回和 GPT 操作均执行。

记录：`doc/evidence/zero3e-sd-image.json`。实体写卡和板端启动仍等待目标设备确认。
