# FatFs FAT32 / exFAT 主机验证

在 Windows PowerShell 运行：

```powershell
python tests/fatfs_dual_format/run_tests.py
```

默认读取项目局部 `overlay-apps/components/fatfs/src` 和当前构建生成的
`C:/esp/hosino-qspi/build-apps-80/config/sdkconfig.h`。可通过 `--fatfs`、
`--sdkconfig` 指定其他同版本来源；`--compiler` 可指定已有 TinyCC。

没有宿主 TinyCC 时，脚本从 [TinyCC 官方发布目录](https://download.savannah.gnu.org/releases/tinycc/)
下载 `tcc-0.9.27-win64-bin.zip`（约 478 KiB），校验固定 SHA-256 后只解压到本目录
`.tools`，不安装、不修改 PATH。`.build` 中的来源副本、可执行文件、JSON 结果均为临时产物。

测试使用实际的 `ff.c`、`ffunicode.c`、`ff.h`、`diskio.h` 和 `ffconf.h` 副本。
保留 heap LFN、UTF-8 API、CP936、文件锁、每文件缓存等配置，设置
`FF_FS_EXFAT=1`，保留 `FF_LBA64=0`。仅替换 FreeRTOS 头和 OS 钩子：互斥使用
Windows critical section，内存钩子使用宿主 malloc，块设备使用进程内稀疏内存页。

同一可执行文件依次验证四个场景：

- 64 MiB，FAT32。
- 64 MiB，exFAT。
- 128 GiB，FAT32。
- 128 GiB，exFAT。

每个场景建立 MBR 首分区并格式化，确认挂载所得文件系统类型，查询空闲簇数；
创建中文目录和混合字符长文件名，以非扇区对齐的数据块写入约 1 MiB、同步、关闭、
重新打开逐字节和 CRC32 校验；再追加、改名、查询长度，卸载并清除原 FATFS 对象，
重新挂载后读回、随机 seek、枚举中文长文件名并删除文件。结束时检查 LFN 临时分配归零。

稀疏设备只有逻辑容量为 128 GiB；实际数据驻留约 1.1 MiB 内存。测试代码未实现主机文件或
物理块设备后端，不接受盘符/物理盘参数，也不会读取、格式化任何真实卡或磁盘。

验证边界：这是 FatFs 核心的顺序功能测试，未覆盖 ESP VFS、SDMMC、40 MHz 信号、
实际 SD 卡、断电恢复、多任务竞争或大于 2 GiB 的文件。ESP VFS 的 `off_t` 在当前
Xtensa 工具链中仍是 32 位；本测试不能用于宣称 POSIX 大文件支持。

本次首次编译发现 exFAT 代码路径将 `FF_USE_LABEL` 用于 C 表达式，而禁用的
`CONFIG_FATFS_USE_LABEL` 未定义；项目局部组件已将该禁用情况正规化为 0。
测试不通过宿主命令行绕过此问题，因而能检查这项真实修复。

启用 exFAT 会把 `FSIZE_t` 扩展为 64 位，并改变 `FATFS`、`FIL`、`FILINFO` 等布局。
固件中所有使用者必须包含同一份局部 `ff.h` / `ffconf.h` 并重新编译；不能混用
旧 FatFs 静态库或旧对象文件。保留 `FF_LBA64=0` 不会维持这些结构的原 ABI。
