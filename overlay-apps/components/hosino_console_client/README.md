# 星野控制台设备客户端（候选版本）

这是 ESP-IDF 5.5.5 的 HTTP 客户端组件，供 Brookesia `HosinoConsole` 的对话、管理双页使用。它不创建自己的显示、音频、I2C、Wi-Fi 或 LVGL 任务。

接口依据为用户提供的 `C:/Users/95863/Desktop/ESP32-API-SUMMARY-20261111.md`，以及 `evidence/console-grok-contract-20261112.json` 中开发者 Bot 对线上源码的转述。旧本地控制台源码没有作为实现依据。Bot 的补充契约未经本组件独立读取服务器源码或已登录请求验证，示例中的数值不是实测数据。

## 范围

- 显式登录/退出，读取登录状态和能力清单。
- 会话列表、显式选择会话、最近 10 条消息；显示文本块，不把 thinking 块当成回答。
- 显式新建设备会话；只在返回 `ok:true`、`runStarted:false` 和 `agent:main:dashboard:` key 后自动选中。
- 已有会话（包括QQ）及设备会话均可显式发送和中止，提交key必须与当前选中key一致。默认`deliver:false`；用户明确选择渠道回复后可`deliver:true`。它只控制自动渠道回复，**不声称禁用了模型的消息发送工具**。
- QQ原文直发使用独立`SendQQ`命令、`qqSend`功能检查和`confirm:true`；UI必须先预览目标及正文。只有成功返回非空`messageId`和`channel:qqbot`才确认完成，不自动重试。
- 概览、最多 32 个模型及截断标记、供应商用量摘要/窗口、最多 6 条语义记忆搜索结果。
- 支持已登录的MiMo语音转草稿；不包含任意文本TTS、CLI SSE、WebSocket、账号/密钥/配置更改。

## 线程与资源

`Client::create()` 创建一个 12 KiB PSRAM 栈的网络 worker。GUI 只调用 `submit()`、`setForeground()` 和非阻塞 `copySnapshot()`。固定 DTO 使用 `allocSnapshot()/freeSnapshot()`，不要放在 GUI 栈上。密码只在输入命令、请求缓冲和底层 HTTP 发送阶段短暂存在；组件永不持久化密码；只有明确启用“记住登录”才由独立内部栈 NVS worker 保存会话 Cookie。上层 UI 负责立即清空自己的密码输入和命令副本。

请求串行执行。队列最多4项，写请求在途或排队时拒绝新的普通写请求，退出优先。用户写命令可取消旧的只读同步，由同一worker清理HTTP后执行，不并发关闭句柄。切会话、离开前台和停机也递增generation；旧请求不能发布到新会话。后台释放响应、JSON和会话大缓存，保留RAM Cookie及有界快照；恢复前台按需分配，内存不足可恢复。

v18增加仅本地的`ResolveUncertain`：须`confirm:true`且session_key同时匹配当前选中和待核对会话，不联网、不清草稿、不产生发送成功ACK。可取消正在进行的只读GET，不能抢占在途POST。GUI在发送结果未确认时仍允许编辑、录音、查看历史及导航；恢复发送必须明确核对。`send_http_status/send_error_code/send_status/send_result_key`保存最近显式发送详情，不随后台轮询覆盖，展示前必须匹配key。任意502仍保守视为结果不明，不把一次服务端过滤原因推广到所有失败。

UI显式命令携带非零`request_id`；`completed_request_id/error`只由同代命令完成后更新，轮询ID为0不覆盖。成功校验AI受理结果或QQ直发结果后单独置`accepted_send_request_id`。UI须同时匹配请求ID、会话key和本次草稿内容才清空草稿，不能凭HTTP空闲或最后状态猜测发送成功。已在网络中发送的请求不能因暂停就假定服务器未执行。

`shutdown()` 异步停机，不在 GUI 等待网络。worker 自己关闭 HTTP、清凭据并释放响应、token、200 项会话缓存及两个内部 Snapshot，然后置 `isStopped()`。发布缓冲释放与 `copySnapshot()` 使用同一 mutex，释放后复制返回 false。上层随后调用 `destroy()`；App 析构也可用 `release()` 将最终清理交给 worker，此后不得访问原指针。没有强杀活跃任务。

`setForeground(false)` 用于暂停，保留 RAM 登录状态和轻量快照，但归还大块网络缓存；`shutdown()` 用于关闭，重新创建时，若用户已启用“记住登录”则异步恢复已保存 Cookie 并先 GET /api/me 验证；未启用或 Cookie 失效时需要重新登录。释放日志只记归还字节与 PSRAM free/largest，用于实机核实，不输出私密内容。关闭只是本机释放，不会自动调用服务端 logout；服务端 logout 仅由退出按钮触发。退出按钮立即屏蔽私密快照、清活动 Cookie，仅临时保留完成该次 logout 所需的请求 Cookie；网络失败明确显示服务端退出未确认。

## HTTP、鉴权与边界

固定主机为 `https://hosino.ume.wiki`。启用 CA 证书 bundle 和主机名校验，不跳过证书验证，不携带 Origin/Referer，不使用 Authorization。所有 POST 带 `X-HS:1`。客户端不自动跟随重定向，因此 Cookie 不会因跳转被转发到另一地址。登录 Cookie 只接受 `hs_sid=` 后准确 64 个十六进制字符，默认不存 NVS，明确勾选“记住登录”后才保存；取消勾选、显式退出及服务器返回未登录会异步清除保存。系统时间应先完成现有网络授时以供 TLS 校验。

使用 IDF 的 async HTTPS perform。该 API 的 body 回调返回值本身不能中断读取，所以取消、超过大小和达到截止时间时，回调只对公开 socket 执行一次 `shutdown(SHUT_RDWR)`；HTTP parser/handle 仍由 worker 在 perform 返回后统一 close/cleanup。没有从 GUI 释放句柄，也没有调用会重连的 cancel_request。

响应最多 256 KiB，JSON 最多 16384 token、24 层嵌套，均在预分配 PSRAM 中解析；无全局 cJSON hooks。该容量用于服务端固定 200 条、每条 30 多个字段的合法会话满页；相对最初 128 KiB / 8192 token 实现多预留 256 KiB PSRAM。超限仍显示可恢复错误。会话网络分页使用 0/200/400，保留一页精简 DTO，再按 8 项展示，避免假定网关支持 offset=8。每页响应仍可能超过 256 KiB，此时明确失败，不崩溃或展示不完整页为完整结果。

POST 永不自动重试。发送的超时、取消后不明结果、5xx、不可解析/过大的 2xx 都标记“结果待确认”，由用户查历史。新建成功体不符合契约或结果不明时提示先查会话列表。发送成功仅表示 `runId` 非空且 `status:started`，不表示模型已完成。当前没有权威聊天完成事件；本组件不伪造最终完成状态。

401 清登录和私密缓存；`/api/me` 的 200 `authed:false` 同样清登录。423/429 保留服务端的错误和等待时间。前台历史轮询约 3 秒（活动）/15 秒（闲置），会话约 60 秒，登录状态约 60 秒；失败也有最小轮询间隔，网络读取失败额外退避。后台不轮询。管理接口只在按钮触发时读取。

日志仅记录方法、HTTP/错误码、字节和耗时，不打印密码、Cookie、聊天正文或完整会话 key。HTTP_CLIENT 库日志限制为 WARN，防止其 DEBUG 头部日志暴露 Cookie。

## 已验证与未验证

### 麦克风转写（新增，待实机与服务端验证）

显式 `TranscribeMic` 命令绑定当前设备会话，由同一个网络 worker 取得独立的
`BSP_EXTRA_AUDIO_OWNER_CONSOLE` 音频会话。ES7210 以 16kHz/16bit 两声道 STD
采集，每次256帧，提取 MIC1 为单声道；WAV 只存在 PSRAM，最多15秒/480044字节，
不依赖SD卡、不记录音频或识别正文。`stopVoice(false)`结束录音后请求转写；
`stopVoice(true)`、切换会话、退到后台或关闭会取消并丢弃结果。只在用户停止或
15秒上限到达时上传，少于0.2秒不上传，没有自动重试。已经发出的网络字节无法
撤回；在途取消会停止后续操作并拒绝接收旧结果，不等于服务器回滚。

接口为 `POST /api/asr/transcribe`，原始 `audio/wav`，沿用RAM中的 `hs_sid`
Cookie、`X-HS: 1`、TLS证书与主机名校验；不发送Origin/Referer/Sec-Fetch-Site。
成功必须是 `ok:true` 和非空、未截断的 `text`。结果通过 `voice_text`、
`voice_session_key`、`voice_result_revision` 提供给界面作草稿，后端不会调用Send。
404明确显示接口尚未部署，503提示服务暂不可用；凭据只在服务端配置。

音频读函数忽略调用者timeout参数，现用codec底层读等待1000ms，可能另有锁等待；
因此取消是非阻塞发起，并非硬实时中断。codec停止失败会保留音频所有权并由原
worker重试，成功之前不释放所有权、不启动另一段录音。音乐/视频重新打开时
原媒体门面显式恢复输出取消静音。

`tests/run_voice_tests.py`编译生产WAV助手并验证RIFF字段、短缓冲哨兵、MIC1有符号
样本及完整240000帧，由Python标准wave解析生成的临时合成文件。详见
`ASR_VALIDATION.json`；没有录入真实声音或调用真实ASR，不代表线上路由和服务凭据已完成配置或联调。

2026-11-12：组件 C/C++ 源文件使用真实 Xtensa ESP32-S3 GCC 14.2.0、当前 IDF 5.5.5 依赖/config 做 syntax-only，均通过且无警告。`tests/console_client/run_tests.py` 执行真实生产 C 协议代码：43 个 JSON 用例、UTF-8/Unicode 和非法 NUL、嵌套/token 边界、短/非法 Cookie、URL/JSON 编码、零化、POST 不确定结果分类，以及 10,000 个确定性随机有界输入均通过。

另按 Grok 补充的完整 30 字段会话样例生成 200 条**全部虚构**的回归数据：compact UTF-8 响应 147,845 字节，实际解析需要 12,207 token。旧的 128 KiB / 8192 token 无法容纳这个合法满页；新容量成功解析并遍历全部 200 条会话。实际生产解析器也通过了恰好 16,384 token 可接受、16,385 token 拒绝且不覆盖哨兵的边界测试。字段中的占位 `kind/status` 没有被当成真实枚举或业务状态。

这些宿主测试不执行 FreeRTOS worker、TLS、HTTP 或 GUI。队列 generation、POST 在途状态、资源回收和 copySnapshot 锁由源码审查核对，**没有声称并发运行测试已经通过**。正式整机编译由唯一构建代理执行；板端登录、真实 HTTP 时序、连续切页/退出、弱网和内存高水位仍待实机验收。网络操作可能因同步 DNS、TLS 步骤或弱网超时延后清理，不能把 1 秒 socket 超时解释成“1 秒内必定 shutdown”。请求设有总期限，但该期限不是对底层同步调用的硬实时保证。

匿名公网 `/api/me` 的 PC TLS 检查仅证明域名的未登录接口可达，不证明 ESP32 的已登录功能、发送、管理或完整设备性能已通过。开发期间没有调用线上登录、发送、创建或其他写接口。

## 记住登录

`setRememberLogin(bool)` 为显式用户设置，`rememberLogin()` 返回当前选择。快照的 `remember_login`、`remember_busy`、`remember_saved`、`remember_error` 表示当前选择和持久化结果；存储状态变化合入可观察 revision。网络 worker 不直接读写 NVS。详见相邻 `hosino_console_auth_store/README.md`：4 KiB 内部栈任务、带代次的保存与一次性恢复、默认关闭、不保存密码、未启用 NVS 加密以及实机验证边界。7 天 Cookie 有效期和服务器重启失效规则不因保存而改变。
