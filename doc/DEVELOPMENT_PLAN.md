# ZenithTerminal 开发计划

## 双平台基础示例已跑通

同一份示例源码通过 CMake presets 生成本机可运行程序和面向 Radxa ZERO 3E
的 AArch64 程序。2026-11-12 已完成本机自检、交叉 ELF/加载器/依赖检查，
以及使用 SDK 运行库的 AArch64 QEMU 用户态自检。上板运行留到设备到货后。
本机构建不依赖 Radxa SDK。构建入口见 [SDK 接入文档](SDK_INTEGRATION.md)。

- BSP 源码放在 `/home/ww/codings/sdks/radxa-rk356x-linux6.1-rkr4`。
- 使用 Rockchip RK356x Linux 6.1 RKR4 SDK，接入 Radxa
  `device-rockchip/rk3566_rk3568-linux-6.1` 和
  `kernel/linux-6.1-stan-rkr4.1-buildroot`，固定实际提交。
- ZERO 3E 板级配置为 `rockchip_rk3566_radxa_zero_3e_defconfig`。
- SDK 提供目标 sysroot、libc、加载器和启动文件，匹配 BSP 的 DRM/MPP 库在
  显示阶段按需构建。Clang 不替换目标 ABI，首版沿用 SDK 的 libstdc++。
- `ZT_RK_SDK_ROOT` 指向 Buildroot 的 `host` 或同布局的已重定位应用 SDK。
  完整 BSP 源码树与应用开发前缀分开管理。
- 应用依赖源码固定在 `3rdparty`，本机与目标架构使用独立构建和 staging。
  superbuild 先构建 host protoc，再构建目标 protobuf、Paho C/C++、SDL3，
  最后配置独立应用子构建；host protoc 与目标 protobuf 使用相同版本。
- 首个示例已覆盖基础 C++ 运行环境、protobuf 序列化往返、SDL3 离屏绘图
  和不依赖在线 broker 的 Paho 调用。

当前验收通过：本机示例执行成功；交叉程序为 AArch64，加载器及符号版本
要求与 SDK 匹配，无宿主机库混入或 RPATH/RUNPATH，QEMU 自检通过。
真实板端的硬件访问与性能在后续上板验收。

## 模块与当前阶段

模块边界、CMake 目标与验收顺序见 [模块划分](MODULES.md)。
已实现 `zenith_display_drm` 探测与 atomic/writeback、`zenith_drm_probe`、
`zenith_ui` 离屏绘图、`zenith_preview` 和 `zenith_terminal` 组装入口。
[DRM 能力探测](DRM_PROBE.md) 与 [双 plane 预览](VKMS_PREVIEW.md) 记录操作和验收。
已新增 `zenith_video`、VAAPI 后端、文件回放和 `zenith_backend` UDP 适配；
业务 core、MQTT 和 MPP 按后续里程碑引入。见 [视频调试](VIDEO_PLAYBACK.md)。

## 视频已接入双 plane，后续推进业务与板端

日常 UI、协议与视频链路开发在本机完成，不等待 RK 板，也不要求本机使用
Radxa 内核。共享应用逻辑、帧描述和 DRM atomic 提交代码，隔离平台后端。

1. NV12 测试图作为视频 plane，SDL3 绘制的透明 ARGB 图像作为 UI plane。
2. 通过 DRM atomic 向 VKMS 提交两个 plane，使用 writeback 采集合成结果。
3. 等待 writeback 完成 fence，再在 SDL3 桌面窗口显示采集结果。预览窗口不
   重新合成视频和 UI。
4. 用本地 HEVC 文件和 `zenith_video` 的 VAAPI 实现替换测试图源。
5. 接入 BUAA UDP 组帧输出和业务 UI；板到货后启用 MPP 与 Rockchip DRM。

2026-11-12：本机 VKMS 线性 NV12/ARGB、overlay、writeback 和真实缓冲提交已验证。
累计补丁及可选核组设置已使 1080p 测试图预览接近 60 FPS，仍有少量长帧，
后续以客户端功能为主，不继续阻塞在 VKMS 尾延迟优化上。

HEVC 文件与 BUAA 格式 UDP 回放已接入 VAAPI：本地循环预览约 59.84 FPS，
UDP 两轮 2400 个 AU 全部组帧和解码，窗口约 59.93 FPS。路径明确为 VAAPI
下载到 CPU NV12、复制进 display 空闲槽，再由 VKMS 合成；尚未验证直接 DMA-BUF
导入、modifier 与同步互通。该路径性能不作为板端零拷贝性能。

当前无需回移植 NV12。若后续遇到确切驱动缺陷，将本机补丁放在
`patches/linux-vkms/`，板端补丁放在 `patches/radxa-bsp/`，同时记录对应内核
提交和复现测试。应用构建不自动替换或安装内核模块。

## BUAA 与解码层边界

BUAA 子模块已切换到 starwey604 fork 并固定 FFmpeg 兼容性修复提交，见
[第三方依赖说明](../3rdparty/README.md)。Zenith 直接复用其 UDP 包头编解码，
在 `zenith_backend` 适配中实现独立压缩 AU 组装。原 BGR API 保持不变，
其软件编解码器通过 FFmpeg 9.0.2 / 4.4.2 回归，但不链接进 Zenith 的 VAAPI 路径。
后续在 fork 中拆分依赖，接入完整客户端 API/MQTT，保持既有调用入口。

```text
HEVC 文件 / BUAA 格式 UDP 组帧
        ↓ 压缩 Access Unit
zenith_video 接口 → zenith_decoder_vaapi / zenith_decoder_mpp
        ↓ 带所有权和同步信息的解码帧
zenith_display_drm：framebuffer 导入、双 plane atomic 提交
        ↓
VKMS writeback 预览 / Rockchip HDMI
```

当前帧包含拥有内存的 NV12 数据、尺寸、pitch、色彩与时间戳。后续 DMA-BUF
路径将补充对象 fd、offset、modifier 和同步描述；显示端停止使用前不得回收解码
buffer。桌面 VAAPI 已复用 FFmpeg 硬解，板端 MPP 待实现。

## 参考

- [Radxa 对应 SDK 获取与接入说明](https://forum.radxa.com/t/buildroot-for-radxa-zero-3-w/22006)
- [Radxa 板级配置](https://github.com/radxa/device-rockchip/tree/rk3566_rk3568-linux-6.1)
- [Radxa BSP 内核](https://github.com/radxa/kernel/tree/linux-6.1-stan-rkr4.1-buildroot)
- [VKMS 文档](https://docs.kernel.org/gpu/vkms.html)
