# 计算器

[官方源代码](https://github.com/espressif/esp-brookesia/tree/b22c488f50bafe53342c8e171081bd736396ef58/apps/brookesia_app_calculator)

保留官方图标、按键布局及有符号项栈的加减乘除/小数/百分号算法。
移植改为 phone App，并增加空输入与除零检查、有限结果检查、每项表达式 96 字符上限，
固定数组替代可增长计算栈。结果保留最多六位小数供继续计算，超出 10^15 显示错误。
历史最多 384 字节，屏幕显示最近一项。没有后台定时器或全局事件。

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
