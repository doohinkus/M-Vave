# Unofficial official FM1 M-Vave 固件

[English](README.md) · 简体中文

8 轨、8 槽采样、16 套 FX DEMO，已含 SDK（二进制库与接口头文件）。

[在线工作台与开发包](https://open.m-vave.com/fm1-workbench) · [官方论坛](https://forum.m-vave.com)

在工作台中点击“源码与 SDK”，下载与线上版本对应的开发包。

## 编译

### macOS / Linux

编译在与 Windows 相同的 PowerShell 脚本中运行，环境为 `linux/amd64` Docker 容器（`build.sh`、`tools/docker/Dockerfile`），输出一致。

1. 安装并启动 [Docker Desktop](https://www.docker.com/products/docker-desktop/)。
2. 下载杰理 Linux 工具链（约 26 MB，一次即可）：

   ~~~sh
   mkdir -p ~/.jieli && curl -fL https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C ~/.jieli
   ~~~

   如装在别处，请将 `FM1_LINUX_TOOLCHAIN` 指到含 `pi32v2/` 的目录。
3. 设置 `FM1_SOURCE_URL`：本次构建对应源码包的 HTTPS 下载地址，包内须含本次修改、许可声明和编译／安装脚本；未配置时，网页构建和打包会停止。
4. 编译：

   ~~~sh
   ./build.sh --web       # 网页预览 -> build/preview/index.html
   ./build.sh             # 固件 -> build/dx7/sdk.elf
   ./build.sh --package   # 固件 + 工作台 -> build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc、
                          # build/official-web/FM1-Official-Open-Source.html
   ~~~

首次运行会构建容器镜像（pwsh + curl）；首次网页编译会把 WASI SDK 27 下载到 `build/tools/`，可设置 `FM1_WASI_SDK` 复用已有安装。容器构建已验证：编译产物与官方发布逐字节一致，仅 32 字节构建标识不同（详见 `AGENTS.md`）。

### Windows

需要 Windows x64、PowerShell、`curl.exe` 和 `tar.exe`。编译固件还需安装[杰理 PI32v2 clang 4.0.1 r3 编译器](https://doc.zh-jieli.com/Tools/zh-cn/dev_tools/dev_env/jl_toolchain.html)，所在目录须含 `bin/clang.exe`。

运行以下命令前，先将环境变量 `FM1_SOURCE_URL` 设置为本次构建对应源码包的 HTTPS 下载地址。包内须含本次修改、许可声明和编译／安装脚本；未配置时，网页构建和打包会停止。

~~~powershell
.\build.bat --web
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat --package
~~~

网页由附带的 C 源码编译。首次编译会下载 WASI SDK 27，可设置 `FM1_WASI_SDK` 复用已有安装。

### 查看预览

~~~sh
open build/preview/index.html                  # macOS：用默认浏览器打开
python3 -m http.server -d build/preview 8000   # 或本地起服务后访问 http://localhost:8000
~~~

页面自包含，运行的是编译时固化的模拟器；改动源码后须先重新编译（`./build.sh --web` 或 Windows 的 `.\build.bat --web`）再打开。页面本身不会自动更新——它是带内嵌构建标识的静态快照。

### 发布到 GitHub Pages

~~~sh
./tools/publish.sh
~~~

每次全新执行打包构建，并把 `build/official-web/FM1-Official-Open-Source.html` 推到 `gh-pages` 分支作为 `index.html`（附 `SHA256SUMS.txt`），输出实际网址和构建标识。首次需在仓库设置中启用 Pages（Settings → Pages → Deploy from branch → `gh-pages`）——一次性手动操作；之后工作台位于 `https://<用户>.github.io/<仓库>/`。发布是幂等的：构建未变化时不产生新提交。

输出：`build/preview/index.html`（预览）、`build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc`（固件）、`build/official-web/FM1-Official-Open-Source.html`（完整工作台）。

硬件驱动在 `app/board/`；音频中断调用 `fm1_engine_render()`，`app/audio_output.c` 处理 USB／蓝牙混音和 USB 录音。

[逐轨调音接口](app/instrument.h)支持自定义调音，暂未提供专用调音编辑界面。

APP 采用 [GPLv3＋SDK 链接例外](LICENSE.md)，另有许可标注的文件保留各自条款。设备引擎以二进制提供；[SDK 条款、第三方许可与贡献记录](THIRD_PARTY.md)。

请备份工程和采样，保存或刷写时保持供电。
