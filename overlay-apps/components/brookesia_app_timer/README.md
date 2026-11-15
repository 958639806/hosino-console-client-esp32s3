# 时钟

[官方源代码](https://github.com/espressif/esp-brookesia/tree/b22c488f50bafe53342c8e171081bd736396ef58/apps/brookesia_app_timer)

上游组件名 Timer，但功能实际是数字/指针时钟，不是倒计时。保留官方图标、
110 px 数字字体，以及 time/localtime_r 系统时间读取和 30/6/6 度时针转换逻辑。
新增小时/分钟指针的小数进度，表盘用 LVGL 基础对象绘制以减少 Flash 资源。
生成数字字库只改 include 及 LVGL 8 专用 cache 条件以兼容 LVGL 9，位图未改。
两种表盘均是应用默认屏幕的子容器，不执行 lv_scr_load。
使用在 run 中创建的 1 秒 LVGL 定时器；暂停/关闭停止刷新，回收由 Brookesia 负责。

只读取系统时间，不启动 SNTP、不改全局时区。系统年份不在 2024–2199 范围时，
显示“系统未校时”和空时间，指针隐藏；不显示编译日期或伪造当前日期。
未从上游移入演示步数、电池百分比等假数据。当前没有时间设置入口，
需要后续系统/联网功能提供真实时间。系统时间看似合理也不保证已经联网校准。

## 来源与许可

移植来源为 Espressif 官方 `esp-brookesia` 的 `release/v0.6`，固定提交
`b22c488f50bafe53342c8e171081bd736396ef58`。原许可证 Apache-2.0 保存在本目录 LICENSE。
使用当前项目的 Brookesia 0.6.0-beta2 phone App 与 LVGL 9.4；不升级内核。

应用使用 Brookesia 默认屏幕与资源回收，提供 run/pause/resume/close/cleanResource。
根据 getVisualArea() 布局，底部 68 px 留给 FPS 与原系统导航手势，标题左右留 44 px。
不更换全局屏幕、显示驱动或全局手势，不访问硬件串口。
CMake 仅依赖已有的 brookesia_core 与 hosino_ui_fonts，WHOLE_ARCHIVE 保留插件注册器。

## 验证边界

源代码与逻辑检查不能代替完整固件构建、真机触摸和反复打开/退出测试。
