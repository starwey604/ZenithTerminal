# 本机 UI 调试视频

`demo-1080p60.mp4` 用于文件播放和 UDP 回放；`demo-1080p60.h265` 是同一码流的
Annex-B 导出，供底层解码检查。应用回放使用 MP4 的时间戳，不直接读取裸流。

- 来源：本地 `sp_vision_25/assets/demo/demo.avi`，原片 MJPEG、1440×1080、30 FPS。
  源仓库的 MIT 声明保留在 `LICENSE.source`（TongjiSuperPower，2025）。
- 选取前 20 秒，输出 1200 帧，1920×1080，60 FPS，HEVC Main 8-bit、无音频。
- 原画面不拉伸；左右各填 240 像素黑边。通过重复帧从 30 FPS 转为 60 FPS，保持原速。
  这份素材适合界面联调，不代表原生 60 个不同画面/秒的最重解码负载。
- BT.601 full-range 矩阵/范围转为 BT.709 limited，输出标记 BT.709。
  原片未完整声明 primaries/transfer，这里的 BT.709 标记用于统一测试约定。
- x265 fast/zerolatency，目标 8 Mbps，VBV 8 Mbps/8 Mbit；无 B 帧，关闭 open GOP，
  每 60 帧一个 IDR，重复 VPS/SPS/PPS，带 AUD。实际视频码率约 7.89 Mbps。
- 原片全文件探测读到 687 帧并有一次 MJPEG overread 警告；这份前 20 秒素材使用
  `-xerror` 成功转码。VAAPI 解码与循环验证见 [运行指南](../../doc/VIDEO_PLAYBACK.md)。

## 下载与重建

两份媒体都由 Git LFS 管理，合计约 39.6 MB。普通 Git 中保存指针，不保存视频 blob。

```bash
git lfs install --local
git lfs pull --include='assets/video/*'
# 需要重建时指定原片和一个尚未包含输出文件的目录：
bash tools/video/prepare-demo.sh /path/to/demo.avi build/new-test-media
```

脚本拒绝覆盖已有输出，离线编码一次即可；正常播放和发送不调用编码器。
媒体已登记到本地 Git LFS 索引。提交/推送时须同时上传 LFS 对象，未推送的本地素材
不能由另一台机器通过 `git lfs pull` 获取。
