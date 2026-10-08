# ScrcpyGUI 长截图派生功能

本项目基于 [barry-ran/QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 `dev` 分支进行二次开发，在保留 QtScrcpy 投屏、控制、多设备、录屏、普通截图、文件传输和按键映射等功能的基础上，增加电脑端兼容拼接长截图。

## 使用长截图

1. 正常连接设备并启动投屏。
2. 在手机页面中滚动到要截取内容的顶部。
3. 点击设备窗口右侧工具栏中普通截图按钮下方的“长截图”按钮。
4. “最大拼接屏数”默认是 `99`；短页面在检测到底部后会自动提前结束。
5. 软件以约 `35%` 的距离缓慢向上滑动，并根据实时画面变化判断何时抓取下一屏，不依赖固定的长时间等待。
6. 完成后图片直接写入系统剪贴板，可在微信、Word、画图等软件中按 `Ctrl+V` 粘贴；电脑端不会自动保存图片文件。

拼接器会自动识别固定顶部和固定底栏，两者各保留一份；相邻画面使用边缘特征和滚动历史共同判断重叠位置，以降低重复表单、卡片页面误匹配的概率。

## 注意事项

- 长截图目前使用 QtScrcpy 的 OpenGL 视频渲染画面；macOS Metal 解码模式暂不支持。
- 使用自定义游戏按键映射时，长截图会拒绝启动，避免自动滑动被映射脚本改写。
- 动态视频、自动刷新的广告、瀑布流和持续变化的悬浮控件可能影响拼接结果。
- 输出高度超过 `60000` 像素时会终止生成，以避免过高的内存占用。
- 截图期间请勿手动滚动、旋转设备或调整投屏尺寸。

## 构建

原“启动 adbd”现为有状态的“开启无线调试 / 关闭无线调试”按钮，详见 [无线调试开关说明](docs/wireless-debugging.md)。

构建要求与上游 QtScrcpy 一致：Qt 5.12+ 或 Qt 6、CMake，以及对应平台的 C++ 工具链。Windows 推荐使用 Qt Creator 打开根目录的 `CMakeLists.txt`，选择 Release 配置构建。

克隆时需要拉取子模块：

```bash
git clone --recurse-submodules <repository-url>
```

本派生项目把 QtScrcpyCore 子模块地址改为公开 HTTPS，因此不要求预先配置 GitHub SSH key。

## 许可证与署名

新增的 scrcpy 手机控制插件联动和真实设备名称，见 [联动使用说明](docs/plugin-bridge.md)。插件优先复用 QtScrcpy 已打开的手机窗口，不启动第二路投屏；手机窗口标题和设备列表优先显示系统中的真实设备名称。

整个派生代码库继续使用上游的 Apache License 2.0。详见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。

拼接算法为独立实现，设计思路参考了 MIT 许可的 [scrollshot](https://github.com/xutianyi1999/scrollshot) 和 [screenStitch](https://github.com/jaflo/screenStitch)。
