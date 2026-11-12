# 1080p 长帧：核组约束显著减少尖峰，仍有少量长帧

**长帧主要不在工作队列里等，而是在 worker 已获得 CPU 后执行变慢。**
后续 A/B 与反向对照确认：只限制 VKMS 合成队列的核组，就能显著减少长帧。
7200 帧无跟踪长跑全程约 **59.96 FPS**，仍有 4 帧超过 25 ms，严格稳 60 尚未通过。

最初的定位只增加计时与分析工具，使用上一轮相同的累计补丁和模块
`C8345625018A850F283555A`，没有改合成算法、绑定 CPU 或改变宿主机调度策略。
环境为 CachyOS `7.3.3-1-cachyos`，1920×1080、真实 NV12 + SDL ARGB 双 plane、
writeback、Wayland OpenGL 预览、`--pipeline --uncapped`。

## 3600 帧的时间分到了哪里

总共运行 3610 帧，排除预先指定的 10 帧预热；3600 帧全部与内核 worker 对齐。
24 个 CPU 的 trace buffer overrun / commit overrun / dropped events 均为零。
以下单位均为 ms，数值为组内平均：

| 区间 | 全部 3600 帧 | 其中 59 个 >25 ms 长帧 |
|---|---:|---:|
| 入队 → worker 开始 | 0.0052 | 0.0417 |
| worker 开始 → 返回（墙钟） | 8.985 | 14.842 |
| worker 被调度离开 CPU | 0.048 | 0.055 |
| worker 驻留 CPU 期间的 IRQ/softirq handler | 0.008 | 0.030 |
| 扣除上述离 CPU / 中断后的执行时间 | **8.930** | **14.757** |
| 应用循环开始 → 此次入队 | 7.004 | 11.616 |

排队最长 0.632 ms，不能解释十几毫秒的尖峰。长帧的执行时间均值增加约 5.8 ms，
调度离 CPU 和已记录中断的增长很小。worker 时间包含合成、CRC、写回及少量管理工作；
扣除后仍包含缓存/内存停顿、实际频率变化、未单列的中断入口开销和 NMI，
不是仅计算像素算术指令的时间。

`commit_ms` 增长也不等于工作队列堵塞：blocking commit 等待显示更新，实际
“应用开始 → 入队”可接近一整个 16.67 ms 刷新周期；真正“入队 → 执行”只有微秒量级。

## 59 个长帧都与当帧或前帧经过较低频率核组相邻

这台机器的 `lscpu` 报告两组最高频率：

- 约 5.30 GHz：逻辑 CPU 0–3、12–15。
- 约 3.35 GHz：其余逻辑 CPU。

这是 CPU 能力信息，不是逐帧实际运行频率。根据 `sched_switch` 重建每个 worker
执行期间的核迁移路径，而不只看它启动时位于哪个 CPU：

| worker 驻留路径 | 帧数 | >25 ms 帧数 | 扣除离 CPU / 中断后的执行均值 |
|---|---:|---:|---:|
| 全程仅经过较高最高频率核组 | 3283 | 7（0.21%） | 8.696 ms |
| 曾经过较低最高频率核组 | 317 | 52（16.40%） | 11.361 ms |

59 个长帧中 **52 个当帧经过较低频率核组**。另外 7 个当帧只在较高频率组执行，
但其前一帧全部经过较低频率组；前帧 worker 耗时 15.90–17.27 ms，再加约
0.79–0.92 ms 的预览/循环收尾，已接近或超过一个刷新周期。

例如：

- 帧 344：核路径 `2 → 0 → 17 → 13 → 3 → 14`，worker 15.937 ms，
  fence 返回后的收尾约 0.847 ms。
- 帧 345：worker 本身只需 8.511 ms，入队后仅等约微秒量级，
  但从本帧开始到入队等了 16.518 ms，整帧达到 25.887 ms。

这个时间关系支持“慢执行让下一帧错过刷新时机”的解释。
后面的核组约束对照进一步支持这一方向；实际频率、缓存状态仍可能贡献剩余尖峰。
不能直接给某个共享 `kworker` 绑核，因为它还可能执行其他设备的工作。

## 跟踪不是长帧现象的主要来源

三次运行使用相同应用和模块，每次均测量 3600 帧：

| 配置 | 循环均值折算 FPS | p99 / ms | >25 ms |
|---|---:|---:|---:|
| 工作队列 + sched_switch | 59.31 | 26.149 | 53 |
| 再加 IRQ / softirq（上述详细分析） | 59.28 | 25.921 | 59 |
| 关闭跟踪的对照 | 59.11 | 26.731 | 66 |

这组样本没有显示跟踪导致主要尖峰；它不能精确估算极小的跟踪开销。
FPS 继续采用原 `1000 / mean(total_ms)` 定义，CSV 格式化/写出在这个区间外。
第二次跟踪的全程墙钟为 3610 帧 / 61.01 s，即 59.17 FPS；无跟踪对照为
3610 帧 / 61.14 s，即 59.04 FPS。严格稳 60 仍未达标。

## 核组 A/B：只改 VKMS 队列的 cpumask

同日依次运行 A1 → B1 → B2 → A2。A 使用默认全部 24 个逻辑 CPU；B 使用
CPU 0–3、12–15，十六进制 mask `00f00f`。未修改应用、两个 plane、输出分辨率、
writeback、窗口呈现方式、全局 workqueue mask 或桌面线程。
所有运行仍为 nice -20、ordered max_active=1、默认 affinity_scope、affinity_strict=0。

| 运行 | 队列 CPU 范围 | 跟踪 | 测量帧数 | 循环 FPS | 全程 FPS | p99 / ms | >25 ms | 最长 / ms |
|---|---|---|---:|---:|---:|---:|---:|---:|
| A1 | 默认全部 | 开 | 3600 | 59.33 | 59.24 | 25.698 | 48 | 39.376 |
| B1 | 较高性能核组 | 开 | 3600 | 60.02 | 59.92 | 18.621 | 3 | 31.992 |
| B2 | 较高性能核组 | 关 | 7200 | 60.04 | 59.96 | 18.329 | 4 | 31.361 |
| A2 | 恢复默认全部 | 关 | 3600 | 58.90 | 58.82 | 27.710 | 85 | 35.407 |

每次另有 10 帧预热，不计入循环统计。全程 FPS 使用程序报告的全部帧数/墙钟时间，
包含预热及循环外开销；不能把循环 FPS 四舍五入到 60 当作稳定性通过。
B2 全程 7210 帧 / 120.24 s。18,000 个测量帧均通过逐帧 writeback 样本校验。

A1/B1 使用同一模块 `83A906F79B5A10086A00254`；随后为避免多设备/多 CRTC
的 sysfs 重名，只将队列名称改为 `vkms-cardN-crtcM`。B2/A2 使用这个最终模块
`697819EF5DB92190CC74579`，两对实验内模块均相同，像素算法和调度 flags 没变。

A1/B1 各 3600 帧全部与 worker 匹配，没有丢事件或不完整 worker：

- A1 有 210 帧经过较低最高频率组，其中 40 帧超过 25 ms；其余核组有 8 个长帧。
- B1 全部 3600 帧的完整驻留路径都在指定核组内，跨出目标 mask 的帧数为零。
- worker 排队均值 **0.0074 → 0.0070 ms**；主要变化不是排队。
- 扣除离 CPU/已记录中断后的执行 p99 **16.700 → 11.788 ms**，最长
  **25.481 → 16.820 ms**。执行均值为 8.838 → 8.922 ms，并未整体下降；收益在尾部。
- B1 的 3 个长帧是 16、2003、2004。其前帧 worker 分别约 16.83、16.80、
  15.59 ms，再加预览收尾；本帧开始到入队约 15.35、15.25、16.57 ms。
  这仍是慢执行/收尾挤过刷新时机后，下一次 blocking commit 等待变长的形态。

较高性能核组约束改善了尾延迟；恢复默认后长帧重新增多，效果不只是顺序热身。
这些是桌面日常负载下的有限时长结果，不是无抖动保证。B2 没有内核跟踪，
不能据用户态 CSV 给它的每一个剩余尖峰指定内核原因。

**保留这个可选设置；下一步针对合成执行本身留出更多余量。**
继续细分 NV12 转换、ARGB 混合、XR24 写回的耗时，优先处理尾部增长最多的段。
本轮不再提高优先级、不引入实时调度，也不将应用提前合成来替代真实双 plane。

### 实现、适用范围与复现

仍只维护原来的一个累计 patch：给 ordered composer queue 加 `WQ_SYSFS`，
并以设备号/CRTC 索引命名。默认不限制 CPU；运行时使用 Linux 自带的
per-workqueue `cpumask`，只重配该队列关联的 worker pool，不给共享 kworker 直接绑核。
属性切换与 ordered 队列顺序由[基线 workqueue 实现](https://github.com/gregkh/linux/blob/v7.3.3/kernel/workqueue.c)管理。
初版尝试从模块调用 workqueue attributes API，但本机内核未导出这些符号，
已改为这个可加载的标准 sysfs 路径，没有保留失败的模块参数接口。

具体临时设置/恢复命令见[补丁说明](../patches/linux-vkms/README.md#可选只为-vkms-选择执行核组)。
机制不依赖 AMD、Intel 或 GPU 型号；`00f00f` 只适用于这台机器的编号，换 Intel/Iris
后应按实际 CPU 拓扑和测试结果选择 mask。Iris 不参与 VKMS 的 CPU 软件合成。
本轮没有修改 Radxa SDK，RK3566 正常 VOP2 显示路径也不需要这个队列设置。

### 验证与原始记录

- [四组汇总及内核分析 JSON](evidence/vkms-affinity-2026-11-12.json)。
- [全部 18,000 个测量帧 CSV](evidence/vkms-affinity-2026-11-12.csv)：A1/B1 附 worker
  分段与核路径，B2/A2 这些字段留空；不把缺少跟踪当作耗时为零。
- 原始跟踪、含预热应用 CSV、队列属性和日志留在 `build/perf/affinity-*`。
- 最终队列名称另做 120 帧跟踪检查：全部匹配、零丢事件、完整路径均在目标 mask。
  这次短跑只确认最终接口，不纳入长跑性能对照。
- 最终模块真实双 plane TEST_ONLY、提交/析构生命周期回归通过；1080p 第 3 帧 BMP
  与系统原模块逐字节一致，包含全部 2,073,600 个 RGB 像素。
- 像素运算未变，沿用原数学/混合/并发测试证据；应用未变，沿用上一轮本机测试、
  RK3566 交叉构建、ELF/SDK 检查与 QEMU self-check 结果。
- 最终模块 `build/perf/vkms-affinity-final/vkms.ko`；未安装系统模块或持久化 CPU 设置。
  已恢复原模块和三块输出布局，确认桌面重新持有 VKMS；跟踪实例和管理员会话已退出。

## 复现：不修改内核算法的事件跟踪

先按[设备交接指南](VKMS_PREVIEW.md)释放 VKMS，并加载
[累计补丁模块](../patches/linux-vkms/README.md)。确认运行中 srcversion，
不要将系统原模块的试跑混入优化模块结果。

管理员终端启动 90 秒采集，输出目录必须尚不存在：

```bash
sudo tools/vkms/trace-workqueue.sh 90 build/perf/my-trace
```

出现 `TRACE READY` 后，在普通用户终端运行客户端：

```bash
./build/native/bin/ZenithTerminal --drm-demo --size 1920x1080 \
  --pipeline --take-over --uncapped --frames 3610 --warmup 10 \
  --profile build/perf/my-profile.csv
```

等待采集脚本退出，再分析：

```bash
python3 tools/vkms/analyze-workqueue.py \
  build/perf/my-trace/events.txt build/perf/my-profile.csv build/perf/my-analysis \
  --cpu-topology build/perf/my-trace/cpu-topology.json
```

脚本创建独立 tracefs instance，使用 `mono` 时钟，结束或收到 SIGINT/SIGTERM 后
关闭采集、保留缓冲统计并移除该 instance。它不加载模块、不交接 DRM、不设置全局
tracer，也不要求读取内核符号地址。入队按 VKMS 队列名过滤；执行开始/结束事件没有
队列名，因此采集这些事件后，离线只选择 `vkms_composer_worker`。sched_switch
只保留涉及 kworker 的切换，另记录 IRQ/softirq handler 事件。
采集工具同时记录模块身份、CPU 拓扑、online/global mask 及已公开的 VKMS 队列属性。
原始日志包含其他内核工作线程的事件元数据，留在本机 `build/perf/`，不纳入仓库。

应用 `--profile` 在原字段后追加 `begin_mono_ns`、`fence_mono_ns`、`end_mono_ns`，
使用 Linux `steady_clock` 的单调时基。这份分析要求启用 writeback；`fence_mono_ns`
是用户态等待返回的时刻，不是内核精确 signal 时刻。worker 可以在 fence 唤醒用户态后
才返回，分析不强行限制其尾部时长。

分析器按 work 对象处理重复入队，按 PID/CPU 重建驻留段，合并嵌套的中断区间避免
重复扣除。无消费者的短 worker（≤50 µs）单列；找不到唯一 worker 的帧不会硬配。
trace 丢事件则拒绝给出结论；没有丢事件时仍报告实际匹配帧数。
`--cpu-topology` 可选，用于提供数字化 MAXMHZ 信息时比较核组。

## 证据与验证

- [逐帧关联 CSV](evidence/vkms-long-frame-analysis-2026-11-12.csv)：全部 3600 帧、
  排队/执行/离 CPU/中断时间与核迁移路径。
- [统计与长帧明细 JSON](evidence/vkms-long-frame-analysis-2026-11-12.json)：环境、
  三组对照、CPU 能力分组及 59 个长帧。
- 本机原始数据：`build/perf/timing-trace-{2,3}/`、`timing-{2,3}.csv`、
  `timing-control.csv`；第一份还未开启中断采集。
- 分析器 6 个确定性回归覆盖重复入队、跨核切换、嵌套中断、提前 fence 唤醒、
  缺少入队、歧义配对和丢事件拒绝；本机 CTest 通过。
- RK3566 重新交叉构建、ELF/SDK 符号检查和 QEMU self-check 通过。
- 结束后已恢复系统原 VKMS、原显示布局，独立跟踪实例与管理员会话已退出。
