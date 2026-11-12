# 用录制视频调试双 plane UI

当前支持两种输入：直接循环读本地 HEVC 文件，或者由本机模拟发送端通过
BUAA/RM2027 UDP 协议回放同一素材。二者都走 VAAPI 硬解、NV12 视频 plane、
SDL 透明 UI plane，再由 VKMS writeback 显示桌面窗口。

## 构建与素材

在仓库根目录执行：

```bash
git submodule update --init --recursive
git lfs install --local
git lfs pull --include='assets/video/*'
cmake --preset native -DZT_ENABLE_DRM=ON -DZT_ENABLE_PREVIEW=ON -DZT_VIDEO_BACKEND=vaapi
cmake --build --preset native
ctest --preset native
```

新增的桌面 FFmpeg 依赖通过宿主机 pkg-config 查找：libavcodec/libavformat ≥60、
libavutil ≥58；需要能打开 VAAPI 的 render node。已验证 CachyOS FFmpeg 9.0.2、
Radeon 890M 的 `/dev/dri/renderD128`。不静默回退软件解码；设备不同用 `--va-device` 指定。
这是原有固定源码依赖之外的显式桌面系统依赖，不带入 RK sysroot。

素材规格、来源和重建脚本见 [assets/video](../assets/video/README.md)。
若工作区里只有 LFS 指针，程序提示运行 `git lfs pull`。

## 最方便的日常入口：本地循环播放

先按 [VKMS 设备交接指南](VKMS_PREVIEW.md)释放虚拟卡，并临时加载
[累计优化模块](../patches/linux-vkms/README.md)。可选设置合成队列核组以减少长帧。
不要将命令指向桌面正在使用的真实 GPU。

```bash
./build/native/bin/ZenithTerminal --drm-demo \
  --video-file assets/video/demo-1080p60.mp4 --video-loop \
  --pipeline --take-over --frames 0
```

窗口关闭、Escape 或 Ctrl-C 退出。去掉 `--video-loop` 会在文件结束时退出并显示最后一帧；
`--frames N` 可限制 UI 提交次数。分辨率从首个解码帧取得，显式 `--size` 必须匹配。
视频模式默认按 60 Hz 调 UI，原测试图模式仍保持原来约 30 FPS 的默认限制。
`--uncapped` 只取消 UI 限速，不改变文件或发送器的时间戳节奏。

`--test-only` 使用真实首个解码帧创建两个 FB；`--capture out.bmp` 保存 writeback。
`--no-writeback --no-preview` 可测提交路径，但不提供像素验收。

## 接收和循环发送：两个终端

释放 VKMS 后先启动接收端；初始等待视频最多 10 秒。出现 `VIDEO RX READY` 后启动发送端。
默认仅绑定/发送到 `127.0.0.1:3334`，不需要 MQTT broker。

```bash
# 终端一：正式客户端的 UDP 输入
./build/native/bin/ZenithTerminal --drm-demo --video-udp 3334 \
  --pipeline --take-over --frames 0

# 终端二：开发工具，不属于板端正式客户端
./build/native/bin/zenith_video_replay \
  --file assets/video/demo-1080p60.mp4 --loops 0
```

`--loops 0` 无限回放，`--loops 2` 发两遍。发送端 `--host/--port` 与接收端
`--video-bind/--video-udp` 配对配置；一个接收端口只由一个进程占用。
停止发送后客户端保持最后一帧，UI 继续更新；发送端重新启动后会等待随机访问帧恢复。

每个完整 Annex-B AU 使用一组帧号，分片带 8 字节大端头，单个 UDP 数据报最多
1400 字节（1392 字节负载）。不是 RTP 或 MPEG-TS，也不能直接用 FFmpeg UDP 输出替代。
发送器只读文件、转换 Annex-B、按 PTS 定时和封包，**不会实时重新编码**。
循环边界从带参数集的 IDR 开始，发送帧号保持连续并按 16 位回绕。

## 数据与模块边界

```text
HEVC MP4 → zenith_video_file → owning Annex-B AU ─────────────────────┐
                               ↓ 模拟发送器                         │
                        BUAA UDP header + fragments                  │
                               ↓                                     │
                 zenith_backend 重组、断流标记 ───────────────────────┤
                                                                     ↓
                      zenith_video::Decoder → zenith_decoder_vaapi
                                             ↓ VA surface
                                      显式下载到 CPU NV12
                                             ↓ owning Frame
                         复制到 display 自有空闲槽 + SDL ARGB UI
                                             ↓
                              同一份 DRM atomic / VKMS writeback
```

- `zenith_video`：HEVC AU、解码接口、拥有内存的 NV12 帧和色彩信息，不含 FFmpeg/DRM 类型。
- `zenith_video_file`：桌面 libavformat + `hevc_mp4toannexb`，输出 AU 和微秒 PTS。
- `zenith_decoder_vaapi`：必须取得 VAAPI surface，再显式下载 NV12。FFmpeg 资源在内部释放；
  返回帧可独立持有，不受下一次解码影响。当前只接收带受支持色彩元数据的 8-bit HEVC。
- `zenith_backend`：直接编译固定 BUAA 子模块的 `udp_header.cpp`；组帧适配参考其
  indexed-fragment/total-length 方案，增加连续分片检查、内存上限、超时、旧包过滤与重启处理。
  公共 API 只传 Zenith AU。没有修改上游 BGR API，也没有链接它的旧 FFmpeg 编解码器。
- 应用播放器线程负责解码和文件定时；只覆盖最新**已解码**帧，显示慢时不积累播放延迟。
  压缩帧出现编号缺口时重置解码器并等待随机访问帧，不随意跳过有依赖的 P 帧。
- DRM 新视频入口要求 COLOR_ENCODING/COLOR_RANGE 属性，按解码元数据配置；测试图仍是
  BT.601 full range。本素材为 BT.709 limited。流中尺寸/色彩发生变化会明确报错并清理，
  当前不动态重配 CRTC。`--state delta` 同样保留首次设置的色彩属性。

当前是**明确选择的桌面 CPU staging 路径**，尚未实现 VAAPI DMA-BUF 导出/导入，
也未验证 modifier 互通。板端 MPP/VOP2 零拷贝仍是后续工作。
BUAA 子模块已切到 starwey604 的 fork，原 BGR 解码器的 FFmpeg 9 兼容性已修复，
并通过 FFmpeg 9.0.2 / 4.4.2 编解码回归。原 BUAA 工具可单独构建；Zenith 仍走
上述 VAAPI 路径。固定提交、构建和验证见 [第三方依赖说明](../3rdparty/README.md)。
MQTT、业务模型和 BUAA 客户端 API 的业务接入尚未实现。

## RK3566 与测试

```bash
cmake --preset rk3566-clang -DZT_ENABLE_DRM=ON -DZT_ENABLE_PREVIEW=OFF \
  -DZT_VIDEO_BACKEND=none -DZT_ENABLE_VIDEO_TRANSPORT=ON
cmake --build --preset rk3566-clang
```

`none` 保留原示例/测试图；MPP 未实现，因此不提供假 MPP 选项或软件回退。
`ZT_ENABLE_VIDEO_TRANSPORT=ON` 可独立构建无 FFmpeg 依赖的 UDP 层及测试，VAAPI 模式自动包含它。
不开启新功能的原 smoke 不要求 BUAA 视频源或 FFmpeg。

正常 CTest 不获取 DRM master，也不要求 GPU。显式硬件解码测试：

```bash
./build/native/app/src/decoder/vaapi/zenith_video_decode_test \
  assets/video/demo-1080p60.mp4 /dev/dri/renderD128
```

2026-11-12 验证：

- 文件 AU 边界、每秒 IDR、跨循环单调时间戳与 EOF；VAAPI 1201 帧跨循环，持有首帧
  不被后续帧覆盖，黑边 Y=16/UV=128、BT.709 limited、drain/reset 通过。
- UDP 字节序、重排、重复、缺片/坏长度、编号回绕、超时及发送端重启单元测试通过。
- 文件预览 1320 次提交跨循环，22.06 s，约 59.84 FPS；全部 writeback 样本通过。
- UDP 两轮 2400 AU 全部组帧和解码，无 gap/invalid/expired；2425 次 UI 提交 / 40.46 s，
  约 59.93 FPS，呈现 2394 个不同视频帧。启动及末尾保持帧包含在提交次数里。
  这些数字不是“每个输入帧都显示”的保证；`unique_presented` 与 `decoded` 单独报告。
- 每次 writeback 用 28 个分布采样点检查 YUV 矩阵/范围和预乘 alpha。真实画面已检查：
  [UDP writeback 截图](evidence/video-hevc-writeback-2026-11-12.png)。
- 原生成图第 3 帧 BMP 与原模块基准逐字节一致；同步/分阶段提交生命周期回归通过；
  非循环 EOF、最终截图、SIGINT 清理通过。
- 本机 CTest、RK3566 编译、ELF/SDK 27 项符号要求检查通过；QEMU 平台自检及
  新 NV12/UDP 单元测试通过。上板硬件运行尚待设备。
- 测试结束已恢复系统原 VKMS、三块输出布局并退出管理员会话。

[本轮指标和验证摘要](evidence/video-playback-2026-11-12.json) 已保存。
原始日志和 profile 在 `build/test-media/`；媒体受 Git LFS 管理，其余临时试验产物不入库。
