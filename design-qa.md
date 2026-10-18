# TimeTrace 1.2.0 界面检查

本版保留已确认的简洁原生界面，代码优化未改变布局与基础交互。

## 布局与组件

- 应用详情的返回、图标、名称、重命名和合并操作固定在底部。
- 概览与详情共用 `drawUsage`，日期、总时长和趋势采用同一布局。
- 正文、标题、大号用时分别使用 16／22／44 DIP；普通控件高 36 DIP，尺寸由 `ui_metrics.h` 共用。
- 使用系统字体、Direct2D／DirectWrite 和缓存的应用图标，关闭窗口释放快照与绘图资源。

## 当前版本的证据

- `docs/overview.png`、`week.png`、`detail.png`、`reminders.png`、`settings.png`：使用独立演示数据生成的原生绘图。
- `docs/verification/v120-native-*.png`：实际窗口、提醒编辑、重命名、合并和添加提醒弹窗。
- `docs/verification/v120-lifecycle-final.json`：19 次原生窗口开关及操作读回结果。

原生检查覆盖窗口控制、键盘导航、提醒修改、设备开关、添加应用、重命名、合并、最小窗口滚动及取消清除历史。先预热 7 次，再测量 12 次；GDI 句柄 61 → 61，USER 句柄 26 → 25，关闭后的私有工作集约 14.6 MB。检查均通过。

正式资源长测、干净系统安装、真实通知按钮和多显示器混合 DPI 的待验收项见 [测试报告](docs/TEST-REPORT.md)。
