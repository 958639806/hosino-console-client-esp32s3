# 哔哩桥接和实时转码服务

可选的独立 Python 服务，供本仓库 ESP32 应用使用。它负责首页/搜索/详情、手机扫码登录、个人资料和收藏接口，以及把原始音视频转成设备支持的 MJPEG + PCM。它不提供公共账号或现成可用的共享令牌。

这里的后端源码取自 2026-11-15 实际部署版本；**没有混入尚未部署的自适应帧率候选**。标清目标为 320×180 / 20FPS，清晰为 480×270 / 15FPS。视频预算可选 800/1200/1800/2500/3500kbps 或推荐；PCM 和协议开销另计，复杂内容可能在最低 JPEG 质量下仍超过预算。推荐档不是实时自适应。

## Linux 准备

需要 Python 3、编译工具，以及项目适配过的 FFmpeg / ffprobe。下列命令在自己的部署目录执行，不要直接覆盖已有生产服务。

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r backend/requirements.txt
python tools/prepare_ffmpeg.py --root "$HOME/hosino-ffmpeg-build" --jobs 2
```

`prepare_ffmpeg.py` 获取固定版本 FFmpeg 源码，校验源文件并生成私有 MJPEG-in-FLV 编码标识补丁；有 GPG 时还验证签名。它不安装系统软件、不配置 systemd、不修改防火墙。普通发行版 FFmpeg 不具有该兼容输出，不能直接替代。

## 创建私有配置

选择自己的 HTTPS 域名，例如 `https://bili.example.com/bili`。反向代理仅转发到服务的 loopback 端口；启用账户接口时须由可信代理设置 `X-Forwarded-Proto: https`，不能将带该信任选项的服务直接暴露到公网。

下面仅生成本机私有文件，不打印令牌：

```sh
umask 077
mkdir -p private/state
python - <<'PY'
import json, pathlib, secrets
p = pathlib.Path('private')
token = secrets.token_urlsafe(32)
(p/'device-token').write_text(token, encoding='utf-8')
(p/'device-client.json').write_text(json.dumps({
    'base_url': 'https://bili.example.com/bili',
    'device_token': token
}), encoding='utf-8')
PY
```

把示例域名换成自己的地址。`private` 和证书私钥不得进入 Git；账户 Cookie 也由服务端保存在私有目录。

## 启动

```sh
python backend/bridge.py \
  --state-dir "$PWD/private/state" \
  --token-file "$PWD/private/device-token" \
  --ffmpeg "$HOME/hosino-ffmpeg-build/installed/bin/ffmpeg" \
  --ffprobe "$HOME/hosino-ffmpeg-build/installed/bin/ffprobe" \
  --trusted-tls-proxy
```

服务默认仅监听 `127.0.0.1:18791`。例如 Caddy 可使用自己的域名并剥离 `/bili` 前缀：

```caddy
bili.example.com {
    handle_path /bili/* {
        reverse_proxy 127.0.0.1:18791 {
            flush_interval -1
        }
    }
}
```

配置 TLS、访问限制和进程管理后，再进行自己的真实客户端测试。当前服务只允许有限并发播放，不是面向多人共享的转码平台。B站可能调整接口或风控策略；仅访问自己有权限播放的内容，不处理 DRM 解密。

## 配置设备

通过自己的安全渠道，将 `device-client.json` 放到连接板子的电脑。板子正常运行时执行仓库根目录的工具（不进入 BOOT 模式）：

```powershell
python -m pip install pyserial
python scripts/Provision-Bili.py --port COM9 --config-file C:\private\device-client.json
```

工具只接受私有文件，令牌不放在命令行也不回显；设备完成 NVS 保存后返回确认。完成后在哔哩应用内使用手机B站扫码并确认，Cookie 保留在服务端。

`backend/test_backend.py` 和 `backend/test_native_cancel.py` 提供离线测试；`tools` 中的联机探针需要自行指定目标和依赖，不应拿开发者线上实例做负载测试。
