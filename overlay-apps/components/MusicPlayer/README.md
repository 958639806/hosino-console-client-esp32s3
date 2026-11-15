# 音乐

应用壳与图标来源：[Waveshare ESP32-S3-Touch-AMOLED-1.75 官方 MusicPlayer](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/MusicPlayer)，固定提交 `e4344e70c2fa78a13e8a06566507f1ba8af6672a`，Apache-2.0，许可证随附。

播放改用项目共享的 `hosino_media_player`，实际后端为 Espressif ESP Player 1.1.1。
ESP Player 及其依赖的许可独立于本应用，部分底层 codec/extractor 以预编译库分发。
旧 `bsp_extra_player_*` / `audio_player` 不参与本应用播放，不另设一套 FLAC 解码后端。

依次扫描 `/sdcard/music`、`/sdcard/Music`、`/sdcard/hosino_samples`，采用第一个非空目录；扩展名不区分大小写，包含 MP3/WAV/FLAC。
文件列表只是格式候选，具体编码、采样率、通道数仍以 ESP Player 的真实解码结果为准。
扫描期间持短时 SD lease，扫描完成后仅保留文件名字符串；播放文件的 lease 与音频独占由共享 facade 持有。
缺卡、空目录、打开或解码失败均显示中文提示和真实错误状态。

界面保留固定四行分页列表、播放/暂停和前后曲按钮。显示真实播放位置与已知总时长，时长尚未知时为 `--:--`。
不显示假封面、预录频谱、假元数据或固定一分钟进度。
中文布局依据 `getVisualArea()`，底部保留 68 px，标题左右留 44 px，不接管全局手势/显示。

单个 8 KiB PSRAM 控制任务串行执行 ESP Player 命令；GUI 只排队命令并读取应用自有快照，不执行阻塞播放/关闭调用。
应用内暂停/恢复交给 ESP Player；回桌面、切到后台或关闭应用时，停止 UI 刷新并异步停止播放器、交还音频与 SD lease。
恢复页面后需点击播放，从头开始。关闭期间过期播放命令会失效，控制任务持有独立路径副本，不引用已销毁的 UI/文件列表。
关闭失败保留 facade handle/资源并重试，不强删活任务。停止完成前其他音频应用可能显示资源忙。
应用定时器在 `run` 中创建并由 Brookesia 回收；`close` 只暂停并解除 UI 引用，不二次删除。

依赖 `brookesia_core`、`bsp_extra`、`storage_service`、`hosino_ui_fonts`、`hosino_media_player`。
本组件不实现 I2S/codec/SD 驱动。源码与语法检查不等于真机 MP3/WAV/FLAC 播放验证。
