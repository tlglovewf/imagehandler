# ImageHandler 项目协作指南

## 项目概况

- 本项目是 Windows x64 桌面图像处理工具，使用 C++20、Qt 6 Widgets、Qt Concurrent 和 libvips。
- 主要能力包括大图浏览、规则图块合并、图像分割、图元分组、分层 BMP/Job 包导出，以及可选的 Vulkan 点云预览。
- `src/domain` 放置纯数据模型和轻量规则；`src/imaging` 放置图像算法；`src/io` 负责扫描、加载和事务式导出；`src/tasks` 负责异步任务；`src/ui` 负责界面和交互。
- `tests` 使用 Qt Test。修改行为时，应在最接近该行为的现有测试文件中添加回归测试。

## 构建与测试

在仓库根目录使用 PowerShell：

```powershell
cmake --preset vs2022-x64
cmake --build --preset release --parallel
ctest --test-dir build/vs2022-x64 -C Release --output-on-failure
```

- 默认依赖路径来自 `CMakePresets.json`；不要把个人机器的新路径写入源码。
- 可通过 `-DQT_ROOT=...` 和 `-DVIPS_ROOT=...` 临时覆盖依赖位置。
- 没有 Vulkan SDK 或 Qt ShaderTools 时，可以使用 `-DIMAGE_HANDLER_ENABLE_VULKAN_JOB_PREVIEW=OFF`；其余功能仍应可构建和测试。
- 只运行相关测试时使用 `ctest --test-dir build/vs2022-x64 -C Release -R <测试名> --output-on-failure`。
- 交付代码前至少构建受影响目标并运行相关测试；跨模块修改或导出逻辑修改应运行完整测试套件。

## 代码约定

- 保持 C++20、Qt 6 和现有命名/格式风格；不要引入新的框架或依赖，除非任务明确需要。
- 项目定义了 `QT_NO_KEYWORDS`，使用 `Q_SIGNALS`、`Q_SLOTS` 和 `Q_EMIT`，不要使用 `signals`、`slots`、`emit` 关键字。
- UI 文案保持中文；文件格式字段、Job JSON 键名和既有磁盘结构保持向后兼容。
- 尺寸、偏移、像素数、坐标范围和文件大小优先使用 `qint64`/`quint64`，并在转换为 `int` 前显式校验范围。
- 大图处理必须保持按区域读取、延迟求值或流式写入；不要为了实现简单而把整张大图无条件载入内存。
- 耗时图像操作和导出不得阻塞 UI 线程，并应保留进度、取消和对象生命周期保护。
- 跨线程代码不要捕获可能先销毁的裸 QObject 指针；遵循现有 watcher、任务令牌和取消机制。
- 导出继续采用预检、临时文件、备份和提交的事务式流程。失败或取消时不得留下半成品，也不得静默覆盖非受管目标。
- 路径处理使用 `QDir`、`QFileInfo`、`QSaveFile` 等 Qt API；不要手工拼接 Windows 路径分隔符。
- 除非功能要求改变，不要修改导出编号、遍历顺序、坐标原点、背景块跳过和图元复用语义。

## 测试要求

- 修复 bug 时先明确可复现条件，并添加能在修复前失败、修复后通过的回归测试。
- 文件测试在替换、重命名或删除文件前关闭所有 `QFile` 句柄，确保 Windows 下结果可靠。
- 导出测试应同时验证成功产物、报告内容、覆盖行为、失败回滚和陈旧受管文件清理。
- 异步 UI 测试使用 `QTRY_*` 或信号等待，避免依赖固定睡眠时间。
- 涉及极端尺寸或坐标时覆盖边界值、溢出和空输入；避免测试分配与边界值同规模的内存。

## 修改边界

- 不编辑或提交 `build/`、`.vs/`、`out/`、`output/` 等生成物或用户导出内容。
- 工作区可能包含用户尚未提交的修改；只改任务相关文件，不重置、覆盖或整理无关变更。
- 不把本机绝对路径、临时目录或用户数据写入导出报告、测试基线或提交文件。
- 更新功能行为后同步更新 `README.md` 中受影响的使用说明。

## 完成检查

- 检查 `git diff`，确认没有意外生成物或无关改动。
- 运行 `git diff --check`。
- 报告实际执行的构建/测试命令及结果；若因缺少依赖无法验证，明确说明未验证部分和原因。
