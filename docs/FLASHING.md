# 下载和刷机

适用 **Waveshare ESP32-S3-Touch-AMOLED-2.16**，16MB Flash、8MB PSRAM。其他尺寸或引脚不同的板卡不能直接套用。

## 先构建

首个公开版本仅提供源码，**没有预编译固件下载**，原因见 [第三方说明](../THIRD_PARTY_NOTICES.md)。先按 [构建说明](BUILD.md) 生成 `firmware/apps/80mhz/` 中的三段镜像，再在仓库根目录执行下列命令。

显示 QSPI 使用 **80MHz**，这不是屏幕刷新率 80Hz，也不保证所有应用 60FPS。板子在此配置已有使用记录，其他设备仍需自行验证稳定性。

## Windows / PowerShell

安装 Python 和 esptool：

```powershell
py -m pip install esptool==4.13.0 pyserial
```

用数据线连接板子，确认设备管理器端口，例如 `COM9`。无法连接时，按住 BOOT 再插 USB，松开 BOOT。关闭其他占用串口的软件。

在**构建后的仓库根目录**运行（将 COM9 改为自己的端口）：

```powershell
.\scripts\Flash-Firmware.ps1 -Apps -Variant 80 -Port COM9 -Python python -DryRun
.\scripts\Flash-Firmware.ps1 -Apps -Variant 80 -Port COM9 -Python python
```

如果 `python` 命令不在 PATH，请为 `-Python` 指定实际 `python.exe` 路径。脚本会先校验镜像，并在首次刷入前备份当前 16MB Flash。备份可能包含 Wi-Fi、设备登录和账号数据，**请私下保管，不要上传 GitHub**。后续可用 `-ExistingBackup` 指定具有正确 `.sha256` 校验文件的旧备份。

脚本按 `0x0`、`0x8000`、`0x10000` 分别写入 bootloader、分区表和 app，保留本项目既有布局的 NVS/PHY 区域；不会因此把其他布局的数据迁移到新分区。不要在需要保留设置时改用整片擦除。

写入校验完成后，如果串口因 USB 重连报错，先查看前面的各段校验结果。松开 BOOT，完全断电后重新上电；接电池时还需要用电源键关机再开机。下载模式未退出时不会显示桌面，不要反复盲刷。

## 手动三段写入

需要时可直接用 esptool。先自行备份，进入下载模式后执行：

```powershell
python -m esptool --chip esp32s3 --port COM9 --baud 460800 `
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 firmware/apps/80mhz/split/bootloader.bin `
  0x8000 firmware/apps/80mhz/split/partition-table.bin `
  0x10000 firmware/apps/80mhz/split/esp-brookesia.bin
```

生成的合并镜像仅用于摘要核对或需要重新初始化的场景；从 `0x0` 写入合并镜像会覆盖间隙中的 NVS，优先使用三段方式。

## 初次使用

1. 打开设置连接 Wi-Fi。
2. TF 卡支持 FAT32 和 exFAT，当前 SDMMC 为 1bit / 40MHz；不自动格式化。
3. 本地播放器选择卡上的兼容文件。音乐包括 FLAC；视频推荐 MJPEG AVI，不能把普通 H.264/H.265/MPEG4 视频仅改成 `.avi` 后缀使用。
4. 星野控制台和哔哩功能需要另行配置匹配的服务端，不包含公共服务账号。

