# ImageHandler

Windows x64 图像浏览、规则图块合并与大图分割工具。项目使用 C++20、Qt Widgets 和 libvips；大图预览按可视区域异步读取，合并采用延迟求值，分割导出采用流式 BMP 写入。

## 构建

默认依赖位置已经写入 CMake preset：

- Qt 6.8.2：`D:\tools\3rdparty\Qt\6.8.2\msvc2022_64`
- libvips 8.18：`D:\3rd\vips-dev-8.18`

```powershell
cmake --preset vs2022-x64
cmake --build --preset release --parallel
ctest --test-dir build/vs2022-x64 -C Release --output-on-failure
```

生成的程序位于 `build\vs2022-x64\Release\ImageHandler.exe`。构建后会自动部署 Qt 运行库、libvips DLL 和加载模块。

如需覆盖依赖路径：

```powershell
cmake --preset vs2022-x64 -DQT_ROOT=D:/Qt/6.8.2/msvc2022_64 -DVIPS_ROOT=D:/vips-dev-8.18
```

## 使用流程

1. 使用“打开图片”浏览单张 BMP、PNG、JPEG 或 TIFF；或使用“打开目录”扫描当前目录。
2. 目录中的 BMP 图块可按 `前缀_行_列.bmp` 规则合并，行列必须连续、完整且图块规格一致。
3. 对 1-bit 或 8-bit 灰度图点击“分割预览”，复核或修改单元宽高、阈值和前景极性。
4. 选择蛇形 Z 字或顺时针回字顺序后导出。1-bit/8-bit BMP 位深会被保留。

所有耗时操作均在后台执行，并通过窗口底部的阶段信息、进度条和取消按钮反馈状态。
