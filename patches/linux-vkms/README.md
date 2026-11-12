# VKMS 双 plane / writeback 性能补丁

本目录只维护一个累计补丁：`0001-vkms-optimize-yuv-fixed-point-math.patch`，
直接应用到未修改的基线源码，不需要叠加旧版本。

当前 1280×720 NV12 + SDL ARGB 双 plane 的 1200 帧窗口预览平均 **59.15 FPS**，
固定帧整幅 RGB 输出与原模块完全一致。60 Hz 周期下仍有少量长帧，详见
[完整性能记录](../../doc/VKMS_PERFORMANCE.md)。应用需带 `--uncapped`，否则保留约 30 FPS 的默认限速。

## 源码和适用范围

- 基线：[Linux stable v7.3.3 的 VKMS](https://github.com/gregkh/linux/tree/v7.3.3/drivers/gpu/drm/vkms)。
- 构建/运行验证：x86_64 CachyOS `7.3.3-1-cachyos`，Clang 23.1.4。
- 修改 `vkms_formats.c/.h`、`vkms_drv.c`、`vkms_composer.c`；未修改应用接口或 Radxa SDK。
- 本地补丁，尚未提交上游；未覆盖系统模块，也没有安装 DKMS 规则。
- 只用通用整数、查表和内核位扫描接口；不依赖 AMD/Intel GPU 或专用 SIMD。
  换 CPU 架构/内核需要重建，内核源码改变时需检查适用性；Intel/ARM64 模块尚未实机验证。

## 单一补丁包含四项优化

1. **通用 YUV 定点乘法**：保留原有缩放、截断，使用 `fls64()` 查找有效位。
   负数按位取反，零、-1 和 `INT64_MIN` 保持原语义。
2. **NV12 精确查表**：在 configfs/设备注册前，用原有定点计算预生成六种
   BT.601/709/2020 full/limited 矩阵的通道贡献。每表 18 KiB，总共 108 KiB，
   模块初始化后只读，所有 plane 共享，不在每帧分配/重建。
   利用六种标准矩阵相同的 Y 系数和零项，并在相邻采样间复用 UV 贡献。
   保持裁剪、pitch、offset、读取方向和色度位置；其他格式继续使用原 reader。
3. **XRGB8888 按行写回**：每行选择一次格式，循环中直接调用原像素转换函数，
   让编译器内联，移除每像素间接调用成本。其他 writeback 格式走原路径。
   不关闭 retpoline 等内核安全机制。
4. **alpha 端点快速路径**：完全不透明像素直接复制；全零透明像素保留背景 RGB，
   并按原行为将输出 alpha 设为全不透明。其他 alpha/颜色仍使用原混合公式，
   包括 alpha 为零但 RGB 非零的非规范输入。

不修改 atomic/fence/vblank 调度、不提前在应用内合成、不降低颜色精度或输出分辨率。

## 构建和数学验证（不需要管理员权限）

准备与待运行内核匹配的 VKMS 源码和内核开发文件。复现本次上游基线可用：

```bash
mkdir -p build/perf
git clone --depth 1 --branch v7.3.3 --filter=blob:none --sparse \
  https://github.com/gregkh/linux.git build/perf/linux-v7.3.3
git -C build/perf/linux-v7.3.3 sparse-checkout set drivers/gpu/drm/vkms
SOURCE="$PWD/build/perf/linux-v7.3.3/drivers/gpu/drm/vkms"
KBUILD="/lib/modules/$(uname -r)/build"
tools/vkms/build-module.sh "$SOURCE" build/perf/vkms-candidate "$KBUILD"
python3 tools/vkms/tests/check-math.py "$SOURCE" build/perf/vkms-candidate "$KBUILD"
python3 tools/vkms/tests/check-blend.py "$SOURCE" build/perf/vkms-candidate
```

`build-module.sh` 要求输出目录尚不存在，复制源文件后打补丁，以 `--fuzz=0`
拒绝模糊应用；按内核配置选择 Clang 或默认编译器。它不下载源码、不使用 sudo、
不卸载/加载模块，也不执行 `modules_install`。内核模块使用宿主机内核构建系统，
不使用应用的 CMake 或 RK SDK sysroot。

数学测试直接提取原始/修改后的函数，以及原内核的定点乘法实现：

- 位扫描边界值及 100 万个确定性随机 64 位值。
- 六种 BT.601/709/2020 full/limited 矩阵的每个系数，对 65536 个通道值穷举：
  共 3,538,944 个乘法比较。逐项相同且求和不变，覆盖全部 YUV 三元组的结果。
- 60 万组完整 16 位像素转换比较。
- 六种矩阵的全部 `256³` 个 NV12 输入，共 **100,663,296** 组输出比较。
- 实际 NV12 reader 的四种方向、奇偶裁剪起点、不等 pitch、非零 plane offset。
- XRGB8888 写回的全部 16 位通道值及行边界保护。
- 原/新 alpha 混合的全部不透明 16 位 R 值、透明/半透明与非规范输入、未修改区域。

测试启用 AddressSanitizer 和 UndefinedBehaviorSanitizer；blend 测试以 `-fwrapv`
匹配内核原有有符号算术语义。reader 测试只模拟 framebuffer 的 NV12 内存布局，
被测读取、转换和寻址函数直接提取自实际模块源码。

用户态测试的 `fls64` 替身使用编译器位扫描内建函数；模块本身使用 Linux 的架构实现。
测试结果不替代真实 writeback 的逐像素比较。

## 当前构建产物

本次通过运行验证的模块是 `build/perf/vkms-60fps/vkms.ko`，srcversion 为
`DE805DCED822DF126BDD22D`。`vkms-fast` 等旧构建目录只是历史实验产物，后续使用
当前模块或从本补丁重新构建。应用本机/RK3566 构建不自动安装内核模块。

## 临时加载与恢复（在终端执行）

先关闭本次测试客户端，再按 [设备交接指南](../../doc/VKMS_PREVIEW.md) 让桌面释放
VKMS。不要卸载 AMD/Intel 显卡驱动，不停止桌面。保留以下恢复命令再加载候选模块。
未签名的外部模块可能使内核标记 taint；开启签名强制/lockdown 时需要发行版认可的
签名流程，不能关闭安全机制来绕过拒绝。

```bash
# 替换为本次构建的模块路径；失败时立即加载未修改的系统原模块。
sudo modprobe -r vkms && {
  sudo insmod "$PWD/build/perf/vkms-candidate/vkms.ko" enable_overlay=1 enable_writeback=1 ||
    sudo modprobe vkms enable_overlay=1 enable_writeback=1
}
```

新模块会生成新的设备事件，桌面可能重新获取控制权。先运行
`zenith_drm_probe --driver vkms`，确认卡号和 `readlink -f /sys/class/drm/cardN`，
再按设备交接指南让桌面释放**这张虚拟卡**。不能假设重新加载后仍是 card0。

运行前确认 `/sys/module/vkms/srcversion` 与 `modinfo -F srcversion <候选模块>`
一致，避免把原模块的结果误记成优化后数据。这是模块来源识别，不是安全签名验证。

恢复系统模块时也先结束客户端并让桌面释放 VKMS，然后执行：

```bash
sudo modprobe -r vkms && sudo modprobe vkms enable_overlay=1 enable_writeback=1
```

重载通常自动向桌面发送 add 事件；如之前只发送了 remove 而没有重载，则手动
触发对应 VKMS 卡的 add 事件。最后按交接前记录恢复 Virtual-1 的模式、位置和缩放。
系统原模块始终保留在 `/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/vkms/`。
