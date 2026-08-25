# AGENTS.md

## 适用范围

本文件适用于整个仓库。修改子目录中的代码时，同样遵循这些约定。

## 项目概览

ImageHandler 是一个 Windows x64 桌面应用，使用 C++20、Qt 6 Widgets 和 libvips，实现大图浏览、规则图块合并、分割预览与流式 BMP 导出。

主要目录：

- `src/app`：程序入口。
- `src/domain`：跨模块共享的数据模型。
- `src/imaging`：libvips 初始化、图像加载、拼接与图案检测。
- `src/io`：目录扫描、BMP 流式写入和分割导出。
- `src/tasks`：耗时任务的调度、进度与取消。
- `src/ui`：Qt Widgets 界面和大图视图。
- `tests`：Qt Test 自动化测试。

不要手动修改 `build/`、`.vs/` 或 `out/` 中的生成文件。

## 工具链与依赖

- Windows 10/11 x64。
- Visual Studio 2022，安装“使用 C++ 的桌面开发”工作负载。
- CMake 3.24 或更高版本。
- Qt 6.8.2 MSVC 2022 x64，默认路径为 `D:/tools/3rdparty/Qt/6.8.2/msvc2022_64`。
- libvips 8.18，默认路径为 `D:/3rd/vips-dev-8.18`。

优先使用仓库中的 `CMakePresets.json`，不要另建与 preset 冲突的构建目录。依赖安装位置不同时，通过命令行覆盖缓存变量，不要把个人路径提交到新的配置文件：

```powershell
cmake --preset vs2022-x64 -DQT_ROOT=D:/Qt/6.8.2/msvc2022_64 -DVIPS_ROOT=D:/vips-dev-8.18
```

## 构建与测试

在仓库根目录运行：

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset test-debug
```

Release 构建：

```powershell
cmake --build --preset release --parallel
```

可执行文件位于：

- Debug：`build/vs2022-x64/Debug/ImageHandler.exe`
- Release：`build/vs2022-x64/Release/ImageHandler.exe`

每次修改 C++ 代码后至少完成 Debug 构建。修改核心算法、目录解析、图像格式处理、遍历顺序或导出逻辑时，必须运行 `ctest --preset test-debug`。新增或修复行为时，应在 `tests/TestCore.cpp` 中添加回归测试。

## VS Code

仓库的 `.vscode/` 配置使用 CMake Tools 和 C/C++ 扩展：

- `Ctrl+Shift+B` 执行默认 Debug 构建。
- `F5` 构建并使用 MSVC 调试 `ImageHandler`。
- Release 构建和 Debug 测试可从“运行任务”中选择。

保持 VS Code 任务与 `CMakePresets.json` 一致，不要在任务中复制另一套编译参数。

## C++ 与 Qt 约定

- 保持 C++20，并遵循现有代码的命名与文件组织方式。
- 头文件使用 `#pragma once`；能前置声明 Qt 类型时优先前置声明。
- 使用 RAII 和智能指针管理资源。libvips 图像应继续使用项目定义的 `VipsImagePtr`，不要引入裸资源所有权。
- `image_handler_core` 定义了 `QT_NO_KEYWORDS`。Qt 元对象代码使用 `Q_SIGNALS`、`Q_SLOTS` 和 `Q_EMIT`，不要使用 `signals`、`slots` 或 `emit` 关键字。
- 对不会修改对象状态的方法使用 `const`；对重要返回值酌情使用 `[[nodiscard]]`。
- 用户可见文本和错误信息使用 `QString`，文件路径使用 Qt 路径 API，避免依赖当前工作目录。
- 不要在 UI 线程执行图像扫描、加载、拼接、检测或导出等耗时操作。沿用 `TaskManager`、进度回调和 `CancellationToken`。
- 大图处理应保持按需读取或流式写入，避免无必要地将完整图像复制到内存。
- 跨线程传递 Qt 对象前确认其线程安全性和生命周期；共享结果优先使用现有不可变数据模型与智能指针。

## 变更原则

- 修改范围保持聚焦，不顺带格式化或重写无关代码。
- 新增源文件时同步更新根目录 `CMakeLists.txt` 中对应 target 的源文件列表。
- 新增运行时 Qt 模块或 libvips 依赖时，同时检查链接配置和 Windows 部署逻辑。
- 不提交构建产物、DLL、PDB、用户级 IDE 文件或本机专用配置。
- 不降低现有的 1-bit/8-bit BMP 位深保留、任务取消、进度反馈和错误报告能力。

## 完成标准

提交修改前确认：

1. CMake 配置成功，且没有引入新的配置错误。
2. 受影响的 Debug 或 Release target 编译成功。
3. 相关自动化测试通过；无法运行时说明具体原因。
4. 新增文件已加入正确的 CMake target。
5. 没有修改或提交 `build/` 下的生成内容。
