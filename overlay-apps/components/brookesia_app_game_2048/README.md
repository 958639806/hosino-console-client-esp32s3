# 2048

[官方源代码](https://github.com/espressif/esp-brookesia/tree/b22c488f50bafe53342c8e171081bd736396ef58/apps/brookesia_app_game_2048)

保留官方图标、4×4 权重棋盘、四方向移动、合并记分及对象映射算法。
改用应用内方向按钮，保留系统边缘手势。为稳定暂停/退出，位移同步完成，
不用动画结束回调或异步删除。修复同一回合重复合并已合并目标的问题
（例如 2,2,4,0 向左应为 4,4,0,0），补齐游戏结束对首列空格的判断，
随机新块为 90% 的 2 / 10% 的 4。达到 2048 后显示成功，点击重新开始继续新局。
当前最高分只在本次启动内保存，不写 NVS。

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
