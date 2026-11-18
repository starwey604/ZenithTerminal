# RMUL 2027：3V3 步兵 HUD

**默认看图传。** 常用状态放在画面边缘，在瞄准时不需要切页。借鉴
[复旦星云 EGA](https://github.com/ClearWei/RM26_Client) 的按住查看和离线模拟；
RMUL 以控制度分胜负，首页不照搬他们的基地血条、自动切屏和指挥大屏。

| 位置 | 显示内容 |
| --- | --- |
| 顶栏 | 红蓝控制度、比赛阶段和倒计时；标明己方、本机和练习模式。 |
| 中央 | HEVC 图传与简洁准星；仅在准备、开局倒计时、图传停更或结算时显示对应反馈。 |
| 底栏 | 本机血量、热量、缓冲能量和剩余允许发弹量；补给入口与按键提示。 |
| 临时面板 | 比赛中按住 **Tab** 看双方重装、步兵、哨兵的血量，松开即收起。点击 **SUPPLY** 查看金币、17 mm 兑换比例和当前是否可补给；**Esc** 先关面板，再退出预览。 |

赛前默认选中「血量优先 / 冷却优先」，也可以点选「功率优先 / 爆发优先」。
点击 **CONFIRM & START** 锁定选择，进入五秒倒计时。练习局以十倍速推进
五分钟比赛计时（实际 30 秒）；结算保留最终控制度和胜方。练习数据不发往赛事引擎，
补给面板只查看，不扣金币或增加允许发弹量。

规则依据：[联盟赛规则 V1.0.0](https://bbs.robomaster.com/wiki/20204847/1969150)
§3.1.2、§3.3.3–3.3.4、§3.3.7。首版只显示协议明确提供的全局控制度；
三处控制区归属等有独立数据源再显示，不由机器人坐标推断。
步兵 1V1 后续复用 HUD，替换胜负指标，不套用 3V3 的补给和复活机制。

## 本机预览

按[视频调试](VIDEO_PLAYBACK.md)启用 VAAPI、DRM 和 SDL 预览，并按
[VKMS 设备交接](VKMS_PREVIEW.md)确认虚拟卡已释放。直接用本地素材试看：

```bash
./build/native/bin/ZenithTerminal --drm-demo --hud-demo \
  --video-file assets/video/demo-1080p60.mp4 --video-loop \
  --pipeline --take-over --frames 0
```

要验证真实断流，用同一个素材走本机 UDP。先启动终端一，再在收到
`VIDEO RX READY` 后启动终端二：

```bash
# 终端一：接收
./build/native/bin/ZenithTerminal --drm-demo --hud-demo --video-udp 3334 \
  --pipeline --take-over --frames 0

# 终端二：回放
./build/native/bin/zenith_video_replay --file assets/video/demo-1080p60.mp4 --loops 0
```

在比赛阶段停掉回放。最后一张解码画面会保留，约半秒没有新帧后显示
**VIDEO FROZEN**，HUD 与模拟局继续更新。重新启动回放，收到新解码帧后
恢复 **VIDEO LIVE**。模拟裁判状态和实际视频新鲜度各自更新。

视频仍写 NV12 plane，SDL 只绘制预乘 ARGB HUD；VKMS atomic 合成后的
writeback 才送到桌面窗口。RK3566 编译同一套 HUD 与状态代码，桌面回放和
VKMS 预览入口需要 native VAAPI/preview。
