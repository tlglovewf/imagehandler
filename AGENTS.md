# AGENTS.md

## 适用范围与项目概览

本文件适用于整个仓库。ImageHandler 是 Windows x64 桌面应用，使用 C++20、Qt 6 Widgets 和 libvips，支持大图浏览、规则图块合并、分割预览、Job 数据预览及流式 BMP 导出。

- `src/app`：入口；`src/domain`：共享模型。
- `src/imaging`：图像加载、拼接、检测及预览数据处理。
- `src/io`：目录扫描、Job 文件加载和流式导出。
- `src/tasks`：后台任务、进度与取消。
- `src/ui`：Qt Widgets 界面、大图与 Job 预览。
- `tests`：Qt Test 自动化测试。

不手动编辑 `build/`、`.vs/`、`out/` 及 DLL、PDB 等生成文件，也不将生成文件加入版本控制。允许配置、构建和测试工具正常创建、更新 preset 指定目录中的生成文件；这些变化不违反本条。

## 工具链与构建

使用 Windows 10/11 x64、Visual Studio 2022（“使用 C++ 的桌面开发”工作负载）、CMake 3.24+、Qt 6.8.2 MSVC 2022 x64 和 libvips 8.18。Qt 和 libvips 的默认路径分别为 `D:/tools/3rdparty/Qt/6.8.2/msvc2022_64` 和 `D:/3rd/vips-dev-8.18`。

优先使用仓库中的 `CMakePresets.json`，不要创建与 preset 冲突的构建目录：

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset test-debug
```

Release 构建使用 `cmake --build --preset release --parallel`。依赖路径不同时，只覆盖缓存变量，不新增本机专用配置：

```powershell
cmake --preset vs2022-x64 -DQT_ROOT=D:/Qt/6.8.2/msvc2022_64 -DVIPS_ROOT=D:/vips-dev-8.18
```

可执行文件位于：

- Debug：`build/vs2022-x64/Debug/ImageHandler.exe`
- Release：`build/vs2022-x64/Release/ImageHandler.exe`

## 验证要求

- 修改 C++ 后至少完成 Debug 构建；涉及 Release 行为时追加 Release 验证。
- 修改算法、目录/Job 解析、图像格式、遍历顺序或导出逻辑时，运行 `ctest --preset test-debug`。
- 新增或修复行为时，在 `tests/` 中对应的测试文件添加回归测试。
- 仅修改文档时无需构建。验证受阻时，先在现有授权范围内排查可恢复原因，并继续不受阻的工作。仍无法验证时，列明已完成内容、未验证项目、具体阻塞原因和下一步，不将“代码已修改”表述为“验证已完成”。

## VS Code

仓库的 `.vscode/` 配置使用 CMake Tools 和 C/C++ 扩展：

- `Ctrl+Shift+B`：执行默认 Debug 构建。
- `F5`：构建并使用 MSVC 调试 `ImageHandler`。
- Release 构建和 Debug 测试：从“运行任务”中选择。

保持 VS Code 任务与 `CMakePresets.json` 一致，不在任务中复制另一套编译参数。

## C++、Qt 与图像处理约定

- 遵循现有命名和文件组织；头文件使用 `#pragma once`，可行时前置声明 Qt 类型。
- 使用 RAII 和智能指针；libvips 图像继续使用 `VipsImagePtr`，不引入裸资源所有权。
- `image_handler_core` 定义了 `QT_NO_KEYWORDS`。Qt 元对象代码使用 `Q_SIGNALS`、`Q_SLOTS` 和 `Q_EMIT`，不要使用 `signals`、`slots` 或 `emit` 关键字。
- 对不会修改对象状态的方法使用 `const`；对重要返回值酌情使用 `[[nodiscard]]`。
- 用户可见文本和错误使用 `QString`；路径使用 Qt API，不依赖当前工作目录。
- 扫描、加载、拼接、检测和导出不得阻塞 UI 线程；沿用 `TaskManager`、进度回调和 `CancellationToken`。
- 大图保持按需读取或流式写入，避免无必要的整图复制。
- 跨线程传递 Qt 对象前确认线程安全和生命周期；优先传递不可变模型与智能指针。
- 不降低 1-bit/8-bit BMP 位深保留、取消、进度反馈和错误报告能力。

## 变更约束

- 修改保持聚焦，不顺带格式化或重写无关代码。
- 新增源文件时加入根目录 `CMakeLists.txt` 中正确的 target。
- 新增 Qt 模块或 libvips 运行时依赖时，同时更新链接与 Windows 部署逻辑。

## 完成标准

交付前，由代理按“验证要求”自行检查；本文件中的“确认”指代理自检，不表示请求用户批准。仅文档修改免构建，其余验证按变更类型适用：

1. 需要构建时，CMake 配置成功，且没有引入新的配置错误。
2. 修改 C++ 时，受影响的 Debug target 编译成功；涉及 Release 行为时追加 Release 验证。
3. 按“验证要求”运行的相关自动化测试通过；受阻时按该节说明未验证项目。
4. 新增源文件已加入正确的 CMake target。
5. 没有手动编辑生成文件，也没有将生成文件加入版本控制；正常配置、构建和测试产生的更新允许保留。
