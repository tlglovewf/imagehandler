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
2. 目录图片列表支持 `Ctrl/Shift` 多选。选中少于两张时合并目录内全部 BMP，否则只合并所选图片；图块按文件名末尾的 `行_列.bmp` 坐标定位，前缀和尺寸可以不同，但源位深必须一致。
3. 坐标存在缺块时可确认继续，缺块和不同尺寸图块的未覆盖区域使用白色背景。合并结果保存在下方会话列表中，可随时切换预览，并可流式导出为 8-bit 灰度 BMP。
4. 对 1-bit 或 8-bit 灰度图点击“分割预览”，复核或修改固定 Rect2D 的分割宽高、阈值和前景极性；分割固定从图像左上角 `(0,0)` 开始，识别值会自动校正到最接近且可整除图像尺寸的宽高。
5. 选择蛇形 Z 字或顺时针回字顺序后导出。1-bit/8-bit BMP 位深会被保留。

所有耗时操作均在后台执行，并通过窗口底部的阶段信息、进度条和取消按钮反馈状态。
