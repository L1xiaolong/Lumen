# Lumen

轻量级跨平台图片浏览、像素检查与同步对比工具，基于 Qt 6 和 C++20。

- 目录树、缩略图、搜索及多文件夹工作区。
- 2～4 图同步缩放和平移，滑动分割与按住 B 覆盖 A 对比。
- RGB/RAW/YUV 像素检查、元数据、直方图及 ROI 统计。
- 相机 RAW、Bayer RAW10/12/16、NV12/NV21/I420/P010。
- JPEG、PNG、BMP/DIB；HEIC/HEIF 依赖系统提供的 Qt 插件。
- 可选 ICC 色彩管理；界面支持中文、英文和亮暗主题。

## 下载与运行

从 [GitHub Releases](../../releases) 下载 macOS Apple Silicon 的 DMG 或 Windows x64 安装器。
macOS 将应用拖入 Applications，Windows 按安装向导安装。也可从命令行打开目录：

```sh
Lumen /path/to/images
```

## 构建要求

- CMake 3.25+、C++20 编译器。
- Qt 6.9+，包含 Core、Gui、Network、Quick、Quick Controls 2、Quick Layouts、Quick Templates 2、Svg、ShaderTools、LinguistTools 和匹配版本的 Gui 私有头文件。
- LibRaw 0.21+、Exiv2 0.28+；LittleCMS 2.x 可选。
- 打包需要 Python 3.9+。macOS 崩溃采集首次构建还需要网络、Git 和 Xcode 工具链。

### macOS

安装 Qt 及图像依赖：

```sh
brew install qt libraw exiv2 little-cms2 pkgconf
./build_macos.sh release
./build_macos.sh package
```

应用生成于 `build/macos-preset-release/src/qml/Lumen.app`，安装包生成于 `dist/`。
运行 `./build_macos.sh --help` 查看自定义 Qt 路径、签名及打包选项。

也可直接使用 CMake：

```sh
cmake --preset macos-release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build --preset macos-release --parallel
```

需要跳过 macOS Crashpad 构建时，在 CMake 配置中添加 `-DMVPVIEW_ENABLE_CRASHPAD=OFF`。
此配置下崩溃采集不可用，日志与诊断导出仍可使用。

### Windows

推荐 MSYS2/UCRT64。在 UCRT64 终端安装依赖：

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-declarative mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-shadertools mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-libraw mingw-w64-ucrt-x86_64-exiv2 mingw-w64-ucrt-x86_64-lcms2 mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-nsis
```

在能访问该工具链的 PowerShell 中执行：

```powershell
$env:MSYS2_UCRT64 = "C:/msys64/ucrt64"
.\build_windows.ps1 -Toolchain msys2 -Mode release
.\build_windows.ps1 -Toolchain msys2 -Mode package
```

或直接编译：

```sh
cmake --preset windows-msys2-release
cmake --build --preset windows-msys2-release --parallel
```

打包生成 `dist/Lumen-<版本>-windows-x64-setup.exe`。脚本也支持 `-Toolchain msvc`；
依赖可通过 `vcpkg.json` 安装，ICC 支持对应 `color-management` feature。

## 分发与许可证

项目原创代码及资源采用 [MIT 许可证](LICENSE)。第三方组件保留各自许可证，见
[第三方声明](THIRD_PARTY_NOTICES.md)。Exiv2 采用 GPL-2.0-or-later，分发包含它的安装包时须满足相应许可及源码交付要求。

打包脚本收集实际运行库的许可证及来源，生成 `RUNTIME_DEPENDENCIES.json`、
`RUNTIME_DEPENDENCIES.md` 和 `licenses/runtime/`。MSYS2 可自动匹配软件包来源；
其他 SDK 的来源可通过 `MVPVIEW_RUNTIME_CATALOG` 指定 JSON 文件。
使用 `python3 scripts/collect_runtime_licenses.py --help` 查看调用参数。
自定义清单的结构与校验规则见 `scripts/collect_runtime_licenses.py` 中的 `load_catalog()`。
