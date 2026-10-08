# scrcpy 手机控制插件联动

本二次开发版提供本机专用、带随机令牌的接口。打开 QtScrcpy 并连接手机后，scrcpy-phone-control 0.2.0 会优先复用这个设备的视频和控制连接。AI 读取的是原生解码帧，不是电脑截图，操作也不经过电脑鼠标键盘；不会再启动另一套手机端编码。

## 使用

1. 启动新版 QtScrcpy，刷新设备、打开目标手机投屏。

   可选启动参数：`QtScrcpy.exe --serial 设备序列号`，只自动连接明确指定且已经 ADB 授权的设备；不指定时不会自动选择手机。

   开发验证时可加 `--new-instance` 显式开启独立实例，不关闭原来的投屏窗口。默认仍为单实例。
2. 在 Codex 更新 scrcpy 手机控制插件，并重新加载本地 MCP 服务。
3. 请求查看手机；多个设备时明确指定设备名称或 serial。
4. phone_doctor 中 qtscrcpy_bridge.available 应为 true；phone_connect 返回 connection_mode=qtscrcpy_bridge。
5. 退出插件会话不会关闭你的投屏。原电脑兼容长截图功能保持不变，最大拼接屏数默认 99，完成只复制到剪贴板。

设备列表和窗口标题首先显示 Android 系统 settings global device_name，读不到时使用 adb devices -l 的 model（下划线转换为空格），再退到服务端名称或 serial。用户设置的昵称作为附加说明保留，旧默认 Phone 不再覆盖真实名称。

## 边界

接口只监听 127.0.0.1 随机端口，发现文件为 Windows 的 %LOCALAPPDATA%/QtScrcpy/plugin-bridge.json。令牌是本地授权凭据，请勿上传分享。同一电脑多个实例默认发现最后启动实例。桥接不是对其他本机账户或已入侵程序的安全隔离。

视频尺寸、帧率沿用 QtScrcpy 的现有设置。macOS Metal 硬件解码目前不支持共享帧；没有帧会报错，不通过电脑截图兜底。QtScrcpy 自身剪贴板同步策略保持不变。控制消息“已发送”不代表业务操作成功，需观察返回图像确认。

指令只针对所选 serial，不参与群控。帧序号来自实际解码帧，静止画面重复读取不会伪造新帧；会返回解码帧年龄。旋转、视频重建、重连使旧坐标无效。手势退出主动抬指；异常退出后最多 12 秒自动释放插件手指。只允许有限按键、触摸、文本和应用包名，不提供任意命令接口。

## 开发

上游 QtScrcpyCore 保持固定子模块提交。本仓库在 patches/qtscrcpy-core-plugin.patch 记录目标设备专用输入 API 和 adb devices -l 解析修改。CMake 配置时仅在可以完整应用时自动应用补丁；已经应用则跳过，冲突时停止，不覆盖用户改动。

本机 JSON 行协议 v1：ping/list/observe 是读取；touch/key/text/start_app 必须传 session_id、capture_epoch、width、height。touch 为 down/move/up。close 只释放该插件会话的手势。任何修改请求都必须携带发现文件令牌。图片以 JPEG base64 返回。
