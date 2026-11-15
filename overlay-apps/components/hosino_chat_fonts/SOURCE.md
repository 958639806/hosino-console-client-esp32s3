# 16px 星野聊天中文字库

源字体为项目已固定的 Noto Sans CJK SC Regular，OFL-1.1 原文见 `OFL.txt`。
源字体与转换器版本、源/输出 SHA-256 见 `font-manifest.json`。

用 `scripts/generate_chat_font.py` 生成：Python GB2312 解码表的全部6763汉字、常用中文标点、当前UI字库字符，以及实际 LVGL 9.4 默认拼音词典中的全部候选字（含该词典的繁体字）。另由 `generate_chat_symbols.py` 生成下述颜文字/单色Emoji扩展。此字库不是完整Unicode，罕见姓名、生僻字和未列入的语言仍可能显示占位符。

公开 `hosino_chat_fonts.h` 中的 `hosino_chat_font_16`。只生成16px、2bpp无压缩位图、不含字距表；回落链为 `hosino_chat_font_16 -> hosino_chat_symbols_16 -> lv_font_montserrat_16`，原ASCII和LVGL符号继续可用。固定UI原来的14/16/18/20px子集保持独立。聊天输入/候选区和正文应显式选用该字体。

数据为静态const，没有应用层整份malloc复制，也不依赖SD卡或FreeType。常量位置由 `CONFIG_SPIRAM_RODATA` 决定：启用时映射页复制到PSRAM，关闭时保留在内存映射Flash中；指令是否放PSRAM独立配置。v12启用了RODATA搬移；v13配置关闭RODATA搬移并保留指令搬移，不应把v12的PSRAM占用描述沿用至v13。脚本可通过 `--measure-build C:/esp/hosino-qspi/build-apps-80`，使用现有编译命令只编译一个临时字体对象，记录真实32位对象静态体积；不会启动整机Ninja、改SDK或写板。最终固件页增长及启动空闲内存仍需正式构建/设备验证。

`validation.json` 检查UTF-8代表句、6763汉字和默认拼音全部候选覆盖、fallback、无压缩2bpp及无应用堆复制；这不是视觉质量或实机输入法验收。新UI中的非GB2312字或更换词典后应重新生成并检查。

本次生成7,204字形，实际 `line_height=18`。当前LVGL默认词典有321项、1,090个不同候选汉字，其中397字不在GB2312汉字范围；这批繁体候选已额外纳入。当前真实Xtensa字体对象的 `.rodata` 为557,359字节（约544.3KiB），`.data/.bss/.text` 均为0，低于600KiB静态数据目标。仅字体数据按64KiB向上取整为9页／589,824字节；实际新增映射页还受链接布局影响，不能直接作为最终设备内存实测值。生成的C源码约3.6MB只是文本表示，不是烧入尺寸。

## 颜文字和单色 Emoji 扩展

扩展新增 **7,749个字形、439,473字节静态rodata（429.2KiB）**，`.data/.bss/.text` 为0；中文和扩展两对象共996,832字节。该数字来自真实Xtensa编译单个临时字体对象，不是完整固件链接或实机空闲内存。Apps当前关闭 `CONFIG_SPIRAM_RODATA`，这些常量留在Flash。未改SD、屏幕、播放器或UI布局。

源为 [Google Fonts](https://github.com/google/fonts/tree/c30b221beb0c1da59d563feb91ec8656a1fa972a/ofl) 固定提交 `c30b221beb0c1da59d563feb91ec8656a1fa972a` 中的 Noto Emoji、Noto Sans Symbols 2、Noto Sans、Noto Sans Kannada/Thai/Georgian/Arabic，以及原有固定Noto Sans CJK SC。各原文件与OFL原文的SHA-256、精确下载地址、新增字形数量在 `symbols-manifest.json`；每个新字体的原始许可证在 `licenses/`。缓存原TTF在项目 `.local/fonts/chat-extra`，不改IDF环境。派生位图使用项目新名称，不冒充未修改的Noto字体。

Noto Emoji 的1,489个cmap项中，1,474个补入扩展，其他已由原字体/ASCII链提供或属于NUL/回车等控制字符。Symbols 2按有效cmap补入；其余覆盖Latin扩展、Greek/Cyrillic、组合重音、Kana、韩文兼容字母、框线/数学符号，以及常见颜文字中的 `ಠ ಥ ง ლ و`。实际样本 `(๑•̀ㅂ•́)و✧`、`(╯°□°）╯︵ ┻━┻`、`ಠ_ಠ`、`ʕ•ᴥ•ʔ`、`😭😂🥰👍❤️🎉🚀` 均逐码检查，非空白符还检查了真实生成位图非空。

仍使用 [lv_font_conv 1.5.3](https://github.com/lvgl/lv_font_conv) 的FreeType采样和LVGL输出模块，项目包装脚本不修改node_modules。所有扩展字形被限制在原聊天18px行盒（baseline=3）：过高的少量位图按面积平均等比缩小，其余必要时仅平移；没有裁掉描画笔画，也不会因字体最大行高撑大聊天行距。所有变换逐字形记录在manifest。`tests/chat_fonts/glyph-preview.png` 是真实LVGL字库位图的2倍最近邻预览，非另一款桌面字体截图。

**能力边界：单色逐码字形，不支持彩色Emoji或GSUB/GPOS组合排版。** VS15/VS16与Emoji tag控制码为零宽；ZWJ被LVGL视为控制码，家庭/职业/肤色/旗帜组合可能拆开显示。Indic/Arabic/组合重音只保证码点存在，不保证完整语言塑形或所有颜文字布局完美。原始正文不经过过滤、替换或删除。

可选 `hosino_chat_format_display(dst, cap, src)` 只供UI制作**显示副本**：ZWJ转为 `+`，肤色修饰转为 `[skin1]`…`[skin5]`，tag码转为 `[tag:XX]`，VS15/16不产生可见字符。其它合法UTF-8字节原样保留；不分配堆、不修改src、不拆断UTF-8，返回所需完整长度，超出容量时以NUL结束。源/目标不可重叠；不得把显示副本写回消息历史、编辑框或发送请求。UI尚未自动接入该可选函数，正常fallback不依赖它。

复现：先运行 `python scripts/generate_chat_font.py --measure-build C:/esp/hosino-qspi/build-apps-80`，再运行 `python scripts/generate_chat_symbols.py --fetch --measure-build C:/esp/hosino-qspi/build-apps-80`；首次需桌面Python的fontTools（本次4.67.0），无需在IDF虚拟环境安装包。`--fetch`只下载固定提交且逐个验SHA；不重新选择版本。`python tests/chat_fonts/run_tests.py` 使用本地已安装TCC测试实际C显示函数，并检查所有7,749个扩展字形行盒/示例位图，生成 `tests/chat_fonts/validation.json`。这是主机与静态验证，完整固件链接、板上视觉和输入法验收仍待主线程执行。
