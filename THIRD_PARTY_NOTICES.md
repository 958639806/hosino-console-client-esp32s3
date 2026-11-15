# 第三方许可与来源

本仓库新增的原创集成代码采用根目录 [Apache-2.0](LICENSE)；已有文件头、组件 LICENSE、字体许可及下列第三方条款优先适用于相应内容。根许可证不替换它们，也不表示整个依赖栈具有统一许可证。

| 内容 | 来源与适用条款 |
| --- | --- |
| 目标板示例、BSP、Brookesia 与移入的应用 | Waveshare ESP32-S3-Touch-AMOLED-2.16 / 1.75 / 1.8、Espressif ESP-Brookesia；Apache-2.0，保留原文件声明及 `licenses/upstream/` |
| LVGL 与 LVGL Benchmark | LVGL；MIT，见 `overlay/components/hosino_lvgl_benchmark/LICENSE.txt`，原库由组件管理器获取 |
| JPEGDEC 与音频播放器适配 | 原始 Apache-2.0 声明及组件内来源说明；本仓库保留适配后的源码 |
| FatFs | ChaN 原许可及 ESP-IDF 文件中的 Apache-2.0 声明，见组件原文件；不要整体重标为 MIT |
| 中文、符号及 emoji 字库 | SIL Open Font License 1.1，见各字体目录的 `OFL.txt` / `licenses/` 和生成清单 |
| ESP Player、GMF 及部分媒体依赖 | **Espressif Modified MIT / `LicenseRef-Espressif-Modified-MIT`** 等各组件原条款，见组件 LICENSE 和 `licenses/apps-v6-media/`；该条款限定与乐鑫产品配合使用，不是普通 MIT |
| Helix MP3 | `chmorgan/esp-libhelix-mp3@1.0.3` 的外层封装与内部库有不同许可；内部 Helix 使用 RealNetworks RPSL / RCSL，见 `licenses/helix-mp3/`，不能仅用外层 Apache-2.0 概括 |
| FFmpeg 服务端工具 | 脚本从 FFmpeg 官方发布获取对应源码并作 MJPEG-FLV 兼容补丁；受 FFmpeg 自身许可约束，本仓库不附带 FFmpeg 二进制 |
| curl_cffi 等服务端依赖 | 按 `requirements.txt` 获取，各依赖的许可证继续适用 |

版本与哈希见 `config/dependencies-apps.lock`、各组件 `SOURCE.md` / `PROVENANCE.json` 和 `source-lock.json`。部分乐鑫媒体实现由注册表以预编译库提供；**这里公开的是项目源码和适配代码，不宣称所有第三方编解码实现都开放源码。**

## 本次发布范围

首个公开预览版以**源码**发布，不提供预编译 `.bin`。当前实机固件确实链接了 Helix MP3，而它的内部许可包含额外的源码提示、对象代码通知及组合条款；这些与其他媒体组件组合后的二进制再分发条件尚未完成核对。保留清单和 SHA-256 用于识别已测试构建，不代表附带了固件下载。

需要重新分发固件或变更目标硬件时，应先核对实际链接依赖及各自原条款，不能只沿用仓库首页显示的 Apache-2.0 标记。

本项目不是微雪、乐鑫或哔哩哔哩的官方产品，名称和标识归各自权利人。

