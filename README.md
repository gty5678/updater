# 通用应用更新器

`updater` 用于更新 Windows、macOS 和 Linux 上以独立目录交付的应用。调用方提供安装目录、更新源和可选的启动入口。同一个更新器可以服务多个应用。

## 构建环境

所有平台都需要 Git、CMake 3.25 或更高版本，以及已 bootstrap 的 vcpkg。建议在仓库外检出与 [vcpkg-configuration.json](vcpkg-configuration.json) 基线对应的 vcpkg 标签 `2026.10.16`。

```sh
git clone --branch 2026.10.16 https://github.com/microsoft/vcpkg.git
```

| 平台 | 工具 | 架构 |
| --- | --- | --- |
| Windows | Visual Studio 2022 或 Build Tools 2022，安装「使用 C++ 的桌面开发」及 Windows SDK | x64 |
| macOS | Xcode Command Line Tools、Ninja | Intel x64、Apple Silicon arm64 |
| Linux | GCC、Ninja、binutils、系统静态链接所需的开发包，以及 vcpkg 所需的 `pkg-config`、`curl`、`zip`、`unzip` 和 `tar` | x64、arm64 |

Windows 在 PowerShell 中进入 vcpkg 目录执行 `.\bootstrap-vcpkg.bat`，然后设置环境变量：

```powershell
$env:VCPKG_ROOT = (Get-Location).Path
```

macOS 和 Linux 在 vcpkg 目录执行：

```sh
./bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD"
```

将 `cmake` 放进 PATH，确保 VS Code 能读取相同的 PATH 和 `VCPKG_ROOT`。可从设置好环境的终端运行 `code <仓库路径>`，也可将变量设置到系统或桌面会话环境中。切换环境后应完全退出并重新打开 VS Code。

## 在 VS Code 调试

1. 打开仓库目录，安装工作区推荐的 C/C++、CMake Tools 扩展。macOS 调试使用 CodeLLDB，Linux 调试还需要 `gdb`。
2. 在「运行和调试」中选择 `Updater (Windows)`、`Updater (macOS)` 或 `Updater (Linux)`，然后按 F5。启动任务会配置并构建对应的 Debug preset。
3. 默认传入 `--help`，可在 `src/main.cpp` 的 `main` 入口下断点。调试实际安装时，将 [launch.json](.vscode/launch.json) 对应配置的 `args` 改为下列参数，每个参数单独占一个数组元素。先创建一个仅用于测试的安装目录。

```json
[
  "--install-dir", "${workspaceFolder}/build/demo-app",
  "--local-package", "${workspaceFolder}/build/demo.zip",
  "--no-start-main", "--payload-root", "."
]
```

可在仓库根目录用 CMake 准备一个可反复调试的本地 ZIP：

```sh
cmake -E make_directory build/demo-app build/demo-payload
echo demo > build/demo-payload/version.txt
cmake -E chdir build/demo-payload cmake -E tar cf ../demo.zip --format=zip -- version.txt
```

也可以从终端执行 `cmake --preset <平台>-debug` 和 `cmake --build --preset <平台>-debug`。平台值为 `windows`、`macos` 或 `linux`。

## 打包单个可执行文件

在目标系统和架构上运行对应脚本。脚本会构建 Release，检查链接方式和 `--help`，通过后写入 `dist`。

| 平台 | 仓库根目录运行 | 产物 |
| --- | --- | --- |
| Windows | `.\scripts\package-windows.ps1` | `dist/windows-x64/updater.exe` |
| macOS | `bash scripts/package-macos.sh` | `dist/macos-x64/updater` 或 `dist/macos-arm64/updater` |
| Linux | `bash scripts/package-linux.sh` | `dist/linux-x64/updater` 或 `dist/linux-arm64/updater` |

macOS 首次构建（Apple Silicon 和 Intel 均支持）：

```sh
xcode-select --install  # 已安装 Xcode / Command Line Tools 时跳过
brew install cmake ninja pkg-config
git clone --branch 2026.10.16 https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
bash scripts/package-macos.sh "$HOME/vcpkg"
```

macOS 脚本也会查找 `VCPKG_ROOT`、仓库内的 `vcpkg`、`~/vcpkg` 和 `~/.local/share/vcpkg` 等常见位置。按当前运行架构选择 `arm64-osx` 或 `x64-osx`，将构建缓存隔离到 `build/macos-release-<架构>`。只生成 `dist/macos-<架构>/updater` 单个可执行文件，直接运行该文件并传入 `--help` 可验证启动。它是供应用调用的命令行程序。构建后可运行 `python3 scripts/test-macos.py`，在临时目录验证安装、权限、启动、保留数据、失败回滚，以及在线下载、重定向、重试和超时。

Windows 打包脚本会自动查找 PATH 中的 vcpkg、仓库内的 `vcpkg`、用户目录下的 `vcpkg`，以及系统盘的 `src/vcpkg` 和 `vcpkg`。也可显式指定：

```powershell
.\scripts\package-windows.ps1 -VcpkgRoot C:\src\vcpkg
```

显式参数或环境变量指向的目录必须包含 `vcpkg.exe` 和 CMake toolchain，系统还需提供 Git 命令。脚本也会自动查找 Visual Studio 随附的 CMake 和 Ninja，并通过 `VCPKG_FORCE_SYSTEM_BINARIES` 让 vcpkg 使用本机工具。依赖注册表固定在 `vcpkg-configuration.json` 指定的版本，vcpkg 会自动获取对应的版本数据库。VS Code 直接运行 CMake preset 时仍需配置 `VCPKG_ROOT`。

vcpkg 提供的第三方库静态链接进产物。Windows 使用静态 MSVC 运行库 `/MT`，脚本用 `dumpbin` 检查导入表，运行时使用 Windows 系统 DLL。Linux 使用 `-static`，脚本通过 `readelf` 检查 ELF 的动态依赖和解释器。macOS 下载使用系统 Foundation 的 `NSURLSession`，通过系统信任证书验证 HTTPS，不构建或链接 curl / OpenSSL；解压等第三方库仍静态链接。脚本用 `otool` 检查动态依赖。Intel 和 Apple Silicon 分别构建。macOS 发布所需的签名与公证应在构建之后按发行渠道处理。HTTPS 验证依赖目标系统可用的信任证书。

## 应用接入

更新器放在应用安装目录之外。安装目录必须已存在，并且只包含这个应用的文件。更新包提供完整的新版本，更新包内文件递归覆盖，包外文件保留；`--keep` 指定的顶层文件或目录即使包内存在也不覆盖。

本地安装包示例：

```sh
updater --install-dir ./demo-app --local-package ./demo.zip --no-start-main
```

在线更新示例：

```sh
updater --install-dir ./demo-app --current-version 1.2.0 \
  --latest-json-url https://updates.example.com/latest.json \
  --main-exe bin/demo --keep data,settings.json --pid 12345
```

Windows 将启动入口改为相应的 `.exe`。macOS 可以指定如 `Demo.app/Contents/MacOS/Demo` 的实际可执行文件。发布包须保留 Unix 可执行位，macOS 应用包需由应用发布流程完成签名。当前支持 ZIP 与 tar.zst 中的普通文件和目录，含符号链接或硬链接的更新包会被拒绝。

- `--pid` 等待应用进程退出，默认超时 120 秒，超时后取消安装。省略 PID 时，调用方负责在更新开始前关闭应用。
- `--keep` 接受逗号分隔的顶层文件或目录名，可重复传入。迁移 DarkEye 时传入原有更新源、`--main-exe DarkEye.exe` 和 `--keep data`。
- `--main-args` 接受一段启动参数。Windows 使用原生命令行语法，macOS 和 Linux 使用空白分隔和双引号，不经过 shell。
- `--payload-root` 指定包内应用根目录。默认尝试识别单层外壳目录。`--payload-root .` 强制使用压缩包根目录，适用于包顶层只有一个目录但该目录应原样安装的情况。
- `--local-package` 跳过版本检查与远程 SHA-256 校验，用于安装已信任的本地包。完整参数见 `updater --help`。

## 发布清单

同一份清单可按目标平台和 CPU 架构选择安装包。包地址应为完整 HTTPS URL。下列哈希仅为格式示例，发布时替换为文件实际 SHA-256。版本使用递增的点分数字，如 `1.3.0`。

```json
{
  "latestVersion": "1.3.0",
  "packages": {
    "windows-x64": {
      "url": "https://updates.example.com/demo-windows-x64.zip",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    },
    "macos-arm64": {
      "url": "https://updates.example.com/demo-macos-arm64.tar.zst",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    },
    "linux-x64": {
      "url": "https://updates.example.com/demo-linux-x64.tar.zst",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  }
}
```

清单还可包含 `macos-x64` 和 `linux-arm64`。不再接受旧式顶层 `package`，`packages` 中每个平台的包都必须提供 SHA-256。

## 失败与恢复

更新器获取安装目录内的 `.update-lock` 后才开始工作，并在正常退出时清理。覆盖安装或应用启动失败时默认尝试回滚。退出码 `0` 表示成功或无需更新，`3` 表示哈希不匹配，`10` 表示缺少版本，`11` 表示本地包无效，`20` 表示已回滚，`21` 表示回滚失败，`22` 表示调用方关闭了回滚。其他错误见日志。

若进程被强制终止，先确认更新器已经退出并检查安装状态和 `.updater_backup`，再处理遗留的锁和工作目录。目录替换由多项文件操作组成，强制终止后可能需要手动恢复。回滚失败或关闭回滚时，备份会保留，具体位置会写入日志。
