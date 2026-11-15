# 从源码构建

首个公开预览版对应 `qspi-apps-23` 的网络接收窗口修复版。源码使用固定版本的 Waveshare 示例，加上三个覆盖层生成可编译项目；不需要 Arduino IDE。当前提供并使用过的是 Windows / PowerShell 工作流。

## 准备环境

需要 Git、ESP-IDF **v5.5.5**、ESP32-S3 工具链及 IDF Python 环境。版本和提交固定在 [source-lock.json](../source-lock.json)。构建目录必须为**无空格的 ASCII 路径**；源码仓库可以放在中文路径。

以下以 `C:\esp` 为例，在 PowerShell 执行；已安装相同版本的用户可以跳过重复安装。

```powershell
git clone https://github.com/958639806/hosino-console-client-esp32s3.git
cd hosino-console-client-esp32s3
.\scripts\Setup-Upstream.ps1

git clone --branch v5.5.5 --recursive https://github.com/espressif/esp-idf.git C:\esp\esp-idf-v5.5.5
$env:IDF_TOOLS_PATH = 'C:\esp\hosino-qspi\tools'
& C:\esp\esp-idf-v5.5.5\install.ps1 esp32s3
```

`Build-Comparison.ps1` 会把 `IDF_TOOLS_PATH` 设置为 `<BuildRoot>\tools`，因此安装工具时也要使用这一位置。若自定义 `BuildRoot`，同时更改上述环境变量。若系统阻止执行 PowerShell 脚本，请按你电脑的脚本执行策略设置开发环境。

## 构建 80MHz 多应用版

```powershell
.\scripts\Build-Comparison.ps1 -MHz 80 -Apps `
  -IdfPath C:\esp\esp-idf-v5.5.5 `
  -BuildRoot C:\esp\hosino-qspi

& C:\esp\hosino-qspi\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe `
  .\scripts\package_firmware.py --apps --mhz 80 --build-root C:\esp\hosino-qspi
```

合并固件、三段镜像、配置及清单位于 `firmware/apps/80mhz/`。建议按 [刷机说明](FLASHING.md) 写入三段镜像。

构建过程会从组件注册表下载依赖；已解析版本见 `config/dependencies-apps.lock`。编译器、ESP-IDF、路径和构建环境差异可能影响二进制摘要，重新构建不保证与发布包逐字节相同。发布包的摘要独立记录在清单中。

## 源码布局

| 路径 | 内容 |
| --- | --- |
| `overlay/` | QSPI 配置、显示缓冲和性能测试基础覆盖层 |
| `overlay-localized/` | Brookesia 中文界面、圆角安全区和基础字库 |
| `overlay-apps/` | 17 个应用、音视频、控制台、触摸和存储适配 |
| `config/` | 固定依赖锁和配置 |
| `tests/` | 主机测试源码；部分测试需要自行准备 TCC、FFmpeg 或 IDF 构建输出 |
| `server/bilibili-bridge/` | 可选的哔哩桥接、转码服务及配套测试 |
| `licenses/` | 第三方许可和来源记录 |

覆盖顺序是固定上游示例 → `overlay` → `overlay-localized` → `overlay-apps`。不要直接修改自动生成的 `C:\esp\hosino-qspi\project-apps` 后再期望修改自动回到仓库。

已提交的字体 C 文件可以直接编译；只有重新裁剪字库时才需要字体生成脚本、Node/fontTools 等额外工具。生成脚本中保留了历史开发工具路径，需要按自己的环境传参或调整。

## 服务端边界

星野控制台需要匹配的 Hosino/OpenClaw 服务端，并不是直接输入任意 OpenClaw 网关地址即可使用。本版控制台的基址及配对页地址仍在 `hosino_console_client.cpp`、`HosinoConsole.cpp` 中固定为开发实例；自建时需要修改并重新编译。仓库不提供该实例的密码、设备令牌或使用授权。MiMo 语音识别由服务端配置和付费 API 支持，密钥不应写入设备源码。

哔哩桥接地址和设备令牌可通过 USB 配置，见 [服务端说明](../server/bilibili-bridge/README.md)。

## 测试说明

首个公开版本的构建和实机摘要见 [验证范围](VALIDATION.md)。仓库中的主机测试不代表真实 I²C、无线链路或设备长时间运行验收；部分历史 runner 默认使用开发机工具路径，先查看各测试 README，再提供所需工具。不要把缺失测试工具误认为固件无法构建。

