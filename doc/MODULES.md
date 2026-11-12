# 模块边界与实施顺序

日常在本机调 UI，板端保持同一套业务与 DRM atomic 提交逻辑。先完成可用的
VKMS 双 plane 预览，再接实际解码、网络和板端硬件；不提前创建空模块。

## 目标与依赖边界

| CMake target | 职责与边界 |
| --- | --- |
| `zenith_core` | 客户端状态、连接状态、命令、遥测与状态转换；不依赖 MQTT/protobuf、MPP、VAAPI、DRM 设备。 |
| `zenith_backend` | 封装 BUAA API，转换内部模型，适配连接与回调；协议类型不进入 core。完整压缩 Access Unit 直接交给 video，core 只接状态与统计。 |
| `zenith_video` | 压缩输入、解码接口、帧描述与资源生命周期；没有具体解码实现。首版可为 INTERFACE 库。 |
| `zenith_decoder_mpp` | MPP 实现，封装 MppFrame 与解码器资源。 |
| `zenith_decoder_vaapi` | 桌面 VAAPI 实现，优先复用 FFmpeg 硬解；AVFrame/VASurfaceID 不进入公共接口。 |
| `zenith_display_drm` | 设备与能力探测、路由、buffer/FB 管理、atomic 提交与同步、可选 writeback；VKMS 与 Rockchip 共用实现。 |
| `zenith_ui` | 接收业务快照与可绘制目标，SDL 离屏绘图；不分配 DRM FB，不执行 commit，不开桌面窗口，不启用 SDL KMSDRM。 |
| `zenith_input` | 必需输入设备到应用事件的映射，按实际需求逐步实现。 |
| `zenith_preview` | 可选桌面库：把已完成的 writeback 图像显示在 SDL 窗口，不重新合成视频与 UI。 |
| `zenith_terminal` | 唯一正式客户端可执行文件，组装模块、事件流与后端。已使用此正式 target，保留 ZenithTerminal 兼容构建目标、输出名称及 smoke。 |
| `zenith_drm_probe` | 独立诊断程序，复用 display 探测 API；默认只查询，不接管显示器。 |
| `zenith_tests` | 初期统一测试可执行文件；CTest 区分无设备单元测试与显式选择设备的集成测试。 |

内部实现先用静态库；不建设动态插件系统。第三方依赖只链接到使用它们的模块。
探测库不依赖 SDL、BUAA 或解码器。UI 与 display 间通过绘制目标传递内存，
由 display 管理可扫描输出缓冲，UI 包装像素地址与 pitch 进行绘制。

## 帧契约

- 压缩输入明确 Access Unit 边界、编码类型、时间戳；接口定义提交、取帧、排空、重置。
- 解码帧包含尺寸、裁剪、色彩信息、DMA-BUF 对象列表、各图像 plane 的
  object 索引/offset/stride/modifier、时间戳、同步和所有权。
- NV12 的 Y/UV 图像 plane 不等于两个 DRM 硬件 plane。
- 提交成功不代表可回收当前扫描帧；被后续提交替换并确认不再使用后才归还。
  writeback 输出在完成 fence 后读取和复用。
- AMD VAAPI 到 VKMS 的导入与 modifier 兼容性单独验证。必要时显式转换到
  桌面线性缓冲，最终仍由 VKMS 合成，不将该路径性能作为板端零拷贝性能。

## 配置方向

按实际实现逐步增加 `ZT_DECODER=none|vaapi|mpp`、`ZT_ENABLE_DRM`、
`ZT_ENABLE_PREVIEW` 和标准 `BUILD_TESTING`。明确选择后端但缺少依赖时直接报错，
不静默换后端。已实现 `ZT_ENABLE_DRM` 与 `ZT_ENABLE_PREVIEW`；`ZT_DECODER` 待解码阶段引入。

本机阶段启用 DRM 与 preview，先不启用解码；之后选择 VAAPI。
板端选择 MPP 与 DRM、不构建 preview。本机 SDL 预览需要窗口后端，板端不需要；
两端都不让 SDL KMSDRM 争抢控制权。

## 推进顺序与验收

1. **探测**：实现 display 的设备、connector/encoder/CRTC/plane、格式/modifier、
   属性查询和 probe。运行本机 VKMS 查询，诊断程序交叉编译为 AArch64。
   能力枚举只提供组合候选，不证明布局可提交；真实 framebuffer 的 TEST_ONLY 留到第 2 步。
2. **双 plane 预览**：生成 NV12 测试图，SDL 绘制透明 ARGB UI，创建真实缓冲，
   完成 atomic TEST_ONLY 和提交，经 VKMS writeback 到桌面预览。
   验收混合、持续换帧、缓冲回收与退出。只加入最小 core 状态。
3. **本地视频**：实现 video 契约与 VAAPI 后端，用本地 HEVC 文件替换测试图；
   验证导出、导入、modifier、同步及必要的显式转换路径。
4. **业务和实机**：fork/拆分 BUAA，接 MQTT/UDP、业务 UI、必需输入和断线恢复；
   板到货后接 MPP 与 Rockchip DRM，并做硬件运行和性能验收。

现有双平台 smoke 始终保留。无设备测试默认可运行；硬件测试不自动占用真实显示器。
探测操作与实际 modeset 分开，probe 不提供未经实现的提交选项。

当前第 1、2 步已完成本机正确性验证并通过交叉构建；实现与限制见 [VKMS 预览](VKMS_PREVIEW.md)。
