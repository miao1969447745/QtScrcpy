# QtScrcpy 手机功能接口

QtScrcpy **4.2.1.5** + scrcpy 手机控制插件 **0.3.0**。这是针对手机的结构化接口，不是电脑鼠标键盘自动化。原有 15 个手机工具保留，新增 7 个工具；Qt 功能目录包含 50 个操作（含查询和任务管理），不是 50 项都已真机验收。

## 覆盖范围

投屏服务、普通触摸/文字/启动 App、Android 按键及组合快捷操作、通知栏、屏幕电源、旋转、剪贴板、普通截图、电脑兼容拼接长图、录屏、摄像头与虚拟显示启动配置、按键映射、sndcpy 音频、文件传输、APK 安装、传统无线调试和连接、设备信息查询，以及明确多设备目标的批量原生操作。

电脑窗口大小/位置/置顶、全局 ADB 启动/停止、任意命令、root、解锁、受保护画面绕过不开放。摄像头/音频仍由手机系统权限和机型能力决定。

## 使用顺序

1. 在托盘菜单彻底退出旧版 QtScrcpy，再运行 4.2.1.5；关闭主窗口可能只是最小化到托盘。解压包直接运行 QtScrcpy.exe，不需要命令窗口。
2. 更新插件到 0.3.0，重新加载本地 MCP 服务；本地宿主需要 Python。调用 `phone_doctor` 和 `phone_devices` 确认连接。
3. 调用 `phone_qtscrcpy_capabilities` 获得实际软件支持的目录。旧版未提供 `feature_api=1` 会明确报错，不会猜测功能已接入。
4. 多设备必须由用户指定 serial。未投屏的设备可通过 `phone_qtscrcpy_action(action="service_start", serial=..., parameters={"options": {...}}, confirmed=true)` 启动，再查询 service_status；starting 不等于成功。
5. 点击/拖拽/输入继续用 `phone_connect → phone_observe → phone_tap/phone_swipe/phone_drag/phone_type_text`。Qt 扩展操作则用 `phone_qtscrcpy_observe(serial)` 获取指定设备图片及 observation_id，再调用扩展工具。
6. 每个观察令牌仅能操作一次、30 秒内有效，设备旋转/重新采集会使旧令牌失效。按图片坐标操作，不使用电脑窗口坐标。
7. 对截图覆盖电脑剪贴板、读取剪贴板、录屏、安装、文件传输、无线设置等动作，只有用户明确授权具体设备、内容及路径后，才能设置 confirmed=true。这个字段只是授权记录，不创造授权。
8. 动作“已发送”仅说明控制通道接受发送；要重新读取图片验证实际结果。断线或取消不能自动重发提交、粘贴、安装等操作。

## 插件入口

| 新工具 | 用途 |
|---|---|
| phone_qtscrcpy_capabilities | 获取 50 项操作、参数限制及排除项 |
| phone_qtscrcpy_observe | 读取指定 Qt 设备的原生解码图片和操作令牌 |
| phone_qtscrcpy_action | 调用目录中的结构化操作 |
| phone_screenshot | 普通截图，覆盖电脑剪贴板，不保存图片 |
| phone_long_screenshot | 自动滑动拼接长图，默认最多 99 屏 |
| phone_qtscrcpy_job | 查询任务或 cancel=true 取消 |
| phone_qtscrcpy_batch | 对 1～20 台明确设备执行同一原生操作，每台必须有自己的最新观察令牌 |

批量请求在发送前验证全部目标；不自动参与 Qt 群控，也不默认作用于所有手机。发送阶段仍可能部分成功，不是可回滚事务；出现 partial=true 后必须逐台检查，不自动重试。

## 长图与任务

先把手机页面滚动到需要截取的顶部，再观察并调用：

```json
{"serial":"用户指定的设备序列号","observation_id":"刚刚返回的令牌","max_frames":99,"confirmed":true}
```

返回 job_id 后，每隔约 0.5～1 秒调用 phone_qtscrcpy_job 查询；不要因首次返回 running 就当作完成或再次启动。终态为 completed / failed / cancelled。completed 且 clipboard_written=true 才表示图片已写入电脑剪贴板；saved_file=false 表示未创建图片文件。

固定顶部、底部各保留一份，中间根据重叠拼接。达到屏数/高度上限可能结束而未到底，应检查 frames、reached_bottom 等返回字段；最多 99 屏、输出高度 60000 像素。滚动动态视频、重复结构、弹性回弹可能影响拼接，结果仍需人工查看。截图期间不要手动滑动、旋转或修改尺寸。

长图、剪贴板读取、文件/安装/查询 ADB 操作返回异步任务。任务仅能由发起它的插件会话查询/取消，终态保留约 10 分钟，最多 32 个；退出/phone_disconnect 会取消该插件未完成的任务，但不关闭 Qt 投屏。取消会抬指或停止本任务进程，不能撤销已发送动作、安装变化或部分复制。ADB 进程完成不等于业务页面完成。

## 50 项操作目录

无参数操作使用 parameters={}。除 device_list、keymap_list、service_stop_all、job_status、job_cancel 外均需明确 serial。完整类型、默认值、边界见配套 phone-features.json / 插件 references/QT_FEATURES.json，两边采用同一目录。

| action | 功能 | parameters | 最新画面 | 具体确认 |
|---|---|---|---|---|
| `device_list` | 设备列表和真实名称 | 无 | 无需 | 按任务授权 |
| `service_status` | 指定设备投屏状态 | 无 | 无需 | 按任务授权 |
| `service_start` | 启动指定设备投屏/摄像头 | `options` | 无需 | 需要 |
| `service_stop` | 停止指定设备投屏 | 无 | 无需 | 需要 |
| `service_stop_all` | 停止明确列出的设备 | `serials`（必填） | 无需 | 需要 |
| `record_start` | 开始录屏（需未启动投屏） | `record_path`（必填）、`record_format` | 无需 | 需要 |
| `record_stop` | 停止录屏并结束该投屏 | 无 | 无需 | 需要 |
| `keymap_list` | 列出本机已有按键映射 | 无 | 无需 | 按任务授权 |
| `keymap_apply` | 应用已有按键映射 | `name`（必填） | 需要 | 需要 |
| `keymap_clear` | 清除指定设备按键映射 | 无 | 需要 | 需要 |
| `wireless_status` | 读取无线调试状态 | 无 | 无需 | 按任务授权 |
| `wireless_enable` | 开启传统 5555 无线调试 | 无 | 无需 | 需要 |
| `wireless_disable` | 关闭传统无线调试 | 无 | 无需 | 需要 |
| `audio_status` | 读取音频状态与所属设备 | 无 | 无需 | 按任务授权 |
| `audio_start` | 启动 sndcpy 音频播放 | 无 | 无需 | 需要 |
| `audio_stop` | 停止所属设备音频 | 无 | 无需 | 需要 |
| `audio_install` | 明确安装配套 sndcpy APK | 无 | 无需 | 需要 |
| `power` | 发送手机电源键 | 无 | 需要 | 需要 |
| `notifications` | 展开通知栏 | 无 | 需要 | 按任务授权 |
| `quick_settings` | 展开快捷设置 | 无 | 需要 | 按任务授权 |
| `collapse_panels` | 收起通知/设置栏 | 无 | 需要 | 按任务授权 |
| `rotate` | 旋转手机显示 | 无 | 需要 | 按任务授权 |
| `copy` | 复制手机选中内容 | 无 | 需要 | 按任务授权 |
| `cut` | 剪切手机选中内容 | 无 | 需要 | 需要 |
| `clipboard_paste` | 粘贴手机剪贴板 | 无 | 需要 | 需要 |
| `back_or_screen_on` | 返回或唤醒手机屏幕 | 无 | 需要 | 需要 |
| `camera_zoom_in` | 摄像头放大 | 无 | 需要 | 按任务授权 |
| `camera_zoom_out` | 摄像头缩小 | 无 | 需要 | 按任务授权 |
| `screen_power` | 控制手机屏幕电源 | `enabled`（必填） | 需要 | 按任务授权 |
| `camera_torch` | 摄像头手电筒 | `enabled`（必填） | 需要 | 按任务授权 |
| `resize_display` | 调整 flex 虚拟显示尺寸 | `display_width`（必填）、`display_height`（必填） | 需要 | 按任务授权 |
| `scroll` | 原生滚轮滚动 | `x`（必填）、`y`（必填）、`horizontal`、`vertical` | 需要 | 按任务授权 |
| `clipboard_set` | 设置手机剪贴板/可选粘贴 | `text`（必填）、`paste` | 需要 | 需要 |
| `clipboard_get` | 读取手机返回的剪贴板文本 | 无 | 需要 | 需要 |
| `screenshot` | 普通截图到电脑剪贴板 | 无 | 需要 | 需要 |
| `long_screenshot` | 电脑兼容拼接长图到剪贴板 | `max_frames` | 需要 | 需要 |
| `push_file` | 传送明确文件到手机 | `local_path`（必填）、`remote_path`（必填） | 无需 | 需要 |
| `pull_file` | 取回明确手机文件到新路径 | `local_path`（必填）、`remote_path`（必填） | 无需 | 需要 |
| `install_apk` | 安装/更新明确 APK | `local_path`（必填） | 无需 | 需要 |
| `show_touches` | 切换显示触摸位置 | `enabled`（必填） | 需要 | 需要 |
| `query_apps` | 查询已安装应用包名 | 无 | 无需 | 按任务授权 |
| `query_ip` | 查询 wlan0 网络地址 | 无 | 无需 | 按任务授权 |
| `query_device` | 查询 Android 系统属性 | 无 | 无需 | 按任务授权 |
| `query_displays` | 查询显示器信息 | 无 | 无需 | 按任务授权 |
| `query_cameras` | 查询摄像头信息 | 无 | 无需 | 按任务授权 |
| `network_connect` | 连接明确无线 ADB 地址 | `address`（必填） | 无需 | 需要 |
| `network_disconnect` | 断开选中的无线 ADB 地址 | `address`（必填） | 无需 | 需要 |
| `job_status` | 查询本插件会话的任务 | `job_id`（必填） | 无需 | 按任务授权 |
| `job_cancel` | 取消本插件会话的任务 | `job_id`（必填） | 无需 | 按任务授权 |
| `android_key` | Android 按键及修饰键组合 | `keycode`（必填）、`metastate` | 需要 | 需要 |

## 启动配置与限制

service_start 的 options 支持：
- 图像：max_size、bit_rate、max_fps、video_source=display/camera、orientation=0/90/180/270、crop=宽:高:x:y、codec_name、codec_options。
- 摄像头：camera_facing=front/back、camera_id；要求 Android 12+。摄像头模式不支持手机触摸/剪贴板/通知栏/电源键等显示屏控制，只开放手电筒、缩放等适用控制；可以读取摄像头画面。
- 显示与录制：display、record、record_path（已有电脑绝对目录）、record_format=mp4/mkv、screen_off、stay_awake、use_reverse。
- 高级显示：display_id、new_display=宽x高[/DPI]、flex_display、vd_destroy_content、vd_system_decorations、display_ime_policy=local/fallback/hide、keep_active、start_app（准确包名）。新虚拟显示要求 Android 10+；flex 必须创建新显示、有预览且无 crop，resize_display 仅适用于 flex。
- 未指定的字段沿用 Qt 界面设置，覆盖仅作用于本次请求，不改保存配置。摄像头请求不继承显示屏专用配置；不兼容参数明确失败。要修改运行中的采集配置，先明确停止再启动，不能无声重连或覆盖录制。
- record_start 要求目标投屏未运行；录制在服务器启动时启用。record_stop 会结束该设备投屏和录像，不是只停止录像而继续预览。
- keymap_apply 只能选择 keymap_list 已有的 .json，不接受任意脚本或文件。自定义映射开启时拒绝自动长图/原生滚轮，以免手势被改写。

## 文件、音频、无线与隐私

- push_file/install_apk 只接受用户指定、实际存在的电脑绝对文件路径；install_apk 必须是 APK，可能更新已有应用。不自动下载，不清数据，不自动授予运行时权限。
- pull_file 只允许新文件路径、父目录必须存在，不覆盖电脑已有文件。远端路径必须是绝对普通路径、不含遍历或 shell 语法；当前只接受字母、数字、下划线、点、斜杠、空格、短横线。远端目录/符号链接行为仍由 ADB 决定；用户应指定普通文件，不用于批量目录导出。push_file 可能覆盖手机同名文件，必须事先说明。
- audio_install 仅在具体确认后安装随包 sndcpy.apk。audio_start 不隐式安装，需要手机亲自批准系统音频捕获；不再运行 cmd/bat，不使用 appops 绕过授权。只有收到 PCM 并成功启动电脑播放后才报告 running。默认同一 Qt 实例只能有一台设备的 sndcpy 音频；audio_status 返回当前所属 serial，停止不能抢占另一台设备。
- wireless_enable/disable 是传统网络 ADB 5555 开关，不是 Android TLS 配对设置。可能短暂重启目标 adbd，影响该手机已有投屏；需要可信局域网和明确确认。返回 changing 后查询 wireless_status 确认最终 mode。
- network_connect 使用明确 host:port；仍需一台已列出的授权设备作为用户选择上下文，不能任意探测网络。network_disconnect 的 address 必须与选中的网络 serial 完全一致。不会断开其他手机、全局停止 ADB。
- clipboard_get 等待真实手机文本回复，不用电脑缓存伪装；同一设备只允许一个待处理读取。scrcpy 回复没有请求关联编号，用户同时复制或系统自动同步仍可能影响结果，敏感内容须由用户接管。
- phone_screenshot/phone_long_screenshot 明确覆盖电脑剪贴板；Qt 自身可能同步手机文字到电脑剪贴板，保留其原设置。图片结果默认不落盘；录屏/用户明确传输的文件则会落盘。
- 带随机令牌的接口仅监听 127.0.0.1。不要上传 %LOCALAPPDATA%/QtScrcpy/plugin-bridge.json，也不要将端口暴露公网。它不是被入侵电脑上的安全隔离。
- 默认不会保存图片/输入日志；手机图片、控件文字、剪贴板读取及查询结果仍会进入 AI 宿主上下文，避免无关隐私。无解码帧/Metal 路径/受保护画面不使用电脑截图或提权绕过。

## 验证状态（2026-10-08）

Windows x64 / Qt 6.8.3 Release 构建；4 组 C++ 回归测试和 49 项 Python 离线测试。新接口通过假桥接与本机隔离 JSON socket 测试，长图采用合成滚动画面验证固定栏及中间内容；合成 H.264 用真实 FFmpeg 解码。这些不是本次全部功能的真机验收。历史 MEIZU 21 只读共享画面验证不能证明新增安装、文件、摄像头、音频、无线和批量操作已生效；宿主重载后的 0.3.0 端到端调用仍需验收。

详细记录见 [测试报告](phone-features-test-report.md)。

