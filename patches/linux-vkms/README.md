# VKMS 双 plane / writeback 性能补丁

本目录只维护一个累计补丁：`0001-vkms-optimize-yuv-fixed-point-math.patch`，
直接应用到未修改的基线源码，不需要叠加旧版本。

当前 **1920×1080** NV12 + SDL ARGB 双 plane / writeback / Wayland OpenGL
窗口预览，3600 个测量帧平均 **59.44 FPS**，p95 **17.792 ms**、p99 **25.638 ms**。
其中 44 帧超过 25 ms，最长 35.236 ms；严格“稳 60 FPS”目标尚未通过。
固定帧全部 2,073,600 个 RGB 像素与系统原模块一致。

应用使用 `--size 1920x1080 --pipeline --uncapped`；
[完整性能记录](../../doc/VKMS_PERFORMANCE.md) 包含对照、原始 CSV 和剩余问题。

## 源码和适用范围

- 基线：[Linux stable v7.3.3 的 VKMS](https://github.com/gregkh/linux/tree/v7.3.3/drivers/gpu/drm/vkms)。
- 构建/运行验证：x86_64 CachyOS `7.3.3-1-cachyos`，Clang 23.1.4。
- 修改 `vkms_formats.c/.h`、`vkms_drv.c/.h`、`vkms_composer.c`、`vkms_crtc.c`、
  `vkms_writeback.c`；不修改 Radxa SDK。应用另外提供兼容原 `present()` 的分阶段提交接口。
- 本地补丁，尚未提交上游；未覆盖系统模块，也没有安装 DKMS 规则。
- 只用通用整数、查表和内核位扫描接口；不依赖 AMD/Intel GPU 或专用 SIMD。
  换 CPU 架构/内核需要重建，内核源码改变时需检查适用性；Intel/ARM64 模块尚未实机验证。

## 单一补丁中的优化与生命周期修复

1. **通用 YUV 定点乘法**：保留原有缩放、截断，使用 `fls64()` 查找有效位。
   负数按位取反，零、-1 和 `INT64_MIN` 保持原语义。
2. **NV12 精确查表**：在 configfs/设备注册前，用原有定点计算预生成六种
   BT.601/709/2020 full/limited 矩阵的通道贡献。每表 18 KiB，总共 108 KiB，
   模块初始化后只读，所有 plane 共享，不在每帧分配/重建。
   利用六种标准矩阵相同的 Y 系数和零项，按共享 UV 的相邻两像素展开循环，复用 UV 贡献。
   保持裁剪、pitch、offset、读取方向和色度位置；其他格式继续使用原 reader。
3. **XRGB8888 按行写回**：每行选择一次格式，循环中直接调用原像素转换函数，
   让编译器内联，移除每像素间接调用成本。其他 writeback 格式走原路径。
   不关闭 retpoline 等内核安全机制。
4. **alpha 端点快速路径**：完全不透明像素直接复制；全零透明像素保留背景 RGB，
   并按原行为将输出 alpha 设为全不透明。其他 alpha/颜色仍使用原混合公式，
   包括 alpha 为零但 RGB 非零的非规范输入。

5. **空色彩流水线直接返回**：没有启用 plane color pipeline 时，不再遍历整行做
   恒等的 UNORM → signed → clamp → UNORM 转换；已启用的流水线保留原行为。
6. **减少中间行遍历**：无 color pipeline 的 NV12 完全不透明，直接读入输出行；
   ARGB8888 则融合解包和混合，保留透明、半透明、非规范预乘输入的原结果。
   原有旋转、裁剪和寻址先完成，其他格式或启用色彩流水线时走原路径。
7. **合成器引用计数**：每个已提交 writeback job 与 CRC reader 各持有引用。
   job 初始化及入队完成后才发布 wb_pending，worker 在同一锁内读取 job 指针。
   上一帧延迟 cleanup 不再关闭下一帧所需的合成器；取消的未提交 job 不释放未持有引用。
   参考 atipre 的[邮件列表提案](https://go.mail-archive.com/dri-devel@lists.freedesktop.org/msg646844.html)，
   在本地基线上适配。无待写回 job、也无 CRC reader 的冗余 tick 不重复合成。
8. **显示工作的普通调度优先级**：有序 composer workqueue 加 `WQ_HIGHPRI`，
   仍是 Linux 普通调度，不使用实时调度、CPU 绑定或关闭系统安全机制。
   同机 3600 帧对照中 >25 ms 长帧从 53 降至 33；这是本轮观察，尚无跨机器保证。

保留 60 Hz vblank、完整颜色精度、真实两个 plane、writeback fence 和 CRC reader 功能。
应用不提前合成视频与 UI，OpenGL 只负责显示已完成的 writeback 图像。

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
python3 tools/vkms/tests/check-color.py "$SOURCE" build/perf/vkms-candidate
python3 tools/vkms/tests/check-fusion.py "$SOURCE" build/perf/vkms-candidate
python3 tools/vkms/tests/check-composer.py build/perf/vkms-candidate
python3 tools/vkms/tests/check-publication.py build/perf/vkms-candidate
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
- 无/有色彩流水线、bypass 和缺失操作状态的原/新结果比较。
- ARGB 融合的 2,097,152 个通道用例、四种方向、offset/pitch 和行边界保护。
- writeback 的初始化/入队先于发布，以及获取失败时正确完成自己的错误 fence。
- 合成器并发首次引用、交叠生命周期、vblank 获取失败、40 万对多线程 get/put。
- 原/新 alpha 混合的全部不透明 16 位 R 值、透明/半透明与非规范输入、未修改区域。

测试启用 AddressSanitizer 和 UndefinedBehaviorSanitizer；blend 测试以 `-fwrapv`
匹配内核原有有符号算术语义。reader 测试只模拟 framebuffer 的 NV12 内存布局，
被测读取、转换和寻址函数直接提取自实际模块源码。

用户态测试的 `fls64` 替身使用编译器位扫描内建函数；模块本身使用 Linux 的架构实现。
测试结果不替代真实 writeback 的逐像素比较。

## 当前构建产物

本次通过运行验证的模块是 `build/perf/vkms-1080-publish/vkms.ko`，srcversion 为
`C8345625018A850F283555A`。`vkms-fast`、`vkms-60fps` 等旧目录是历史产物，后续使用
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
