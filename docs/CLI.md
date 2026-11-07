# 命令行参数

本文对应当前更新器的命令行实现。Windows 使用 `updater.exe`，macOS 和 Linux 使用 `updater`。

不传参数或单独传入 `--help` 时显示帮助。

```powershell
.\dist\windows-x64\updater.exe --help
```

执行更新时需要指定安装目录，并选择在线更新或本地安装模式。默认在更新后启动应用，因此还需要提供启动入口，或通过 `--no-start-main` 关闭启动。

```text
updater --install-dir DIR --latest-json-url URL --current-version VERSION --main-exe PATH
updater --install-dir DIR --local-package FILE --no-start-main
```

所有带值的参数都使用 `--参数名 值` 的写法。路径或参数值包含空格时，需要由调用方 shell 将整个值作为一个参数传入。未知参数和缺少参数值会导致程序报错退出。

| 参数 | 值 | 默认值 | 作用与约束 |
| --- | --- | --- | --- |
| `--help` | 无 | — | 单独使用时显示帮助。无参数启动也显示帮助。 |
| `--install-dir` | 目录路径 | 无 | 更新时必填。必须是已存在的应用目录，可以是绝对路径或相对路径，不能是磁盘或文件系统根目录。 |
| `--latest-json-url` | HTTP 或 HTTPS URL | 无 | 在线模式的更新清单地址，清单格式见 [README](../README.md#发布清单)。 |
| `--current-version` | 当前版本字符串 | 无 | 在线模式必填，如 `1.2.0` 或 `v1.2.0`。按点分数字比较版本，版本应使用递增的数字格式。 |
| `--local-package` | 文件路径 | 无 | 使用本地 ZIP 或 tar.zst 包。指定后优先进入本地模式，跳过在线清单、版本比较和 SHA-256 校验。 |
| `--main-exe` | 安装目录内的相对路径 | 无 | 自动启动开启时必填。例如 `Demo.exe`、`bin/demo`、`Demo.app/Contents/MacOS/Demo`。不接受绝对路径或包含 `..` 的路径。启动入口必须存在于选定的更新包内容中。 |
| `--main-args` | 一段启动参数文本 | 空字符串 | 更新成功后传给主程序。关闭自动启动时不使用该值。引号用法见后文示例。 |
| `--start-main` | 无 | 开启 | 更新完成后启动 `--main-exe` 指定的程序。 |
| `--no-start-main` | 无 | — | 关闭自动启动，可省略 `--main-exe`。 |
| `--keep` | 逗号分隔的名称列表 | 空列表 | 保留安装目录内的顶层文件或目录。例如 `data,settings.json`。可以重复传入，多个列表会合并。 |
| `--pid` | 非负整数 PID | `0` | 在安装前等待指定进程退出。`0` 或省略时不等待，调用方应先关闭应用。 |
| `--wait-timeout-seconds` | 整数秒数 | `120` | 等待进程退出的超时，范围为 `0～86400`。超时后取消安装。 |
| `--download-timeout-seconds` | 整数秒数 | `30` | 下载清单时每次尝试的超时。建议使用正整数，小于 `1` 时按 `1` 秒处理。 |
| `--package-timeout-seconds` | 整数秒数 | `120` | 下载更新包时每次尝试的超时。建议使用正整数，小于 `1` 时按 `1` 秒处理。 |
| `--download-retries` | 整数次数 | `3` | 清单和更新包各自的最大下载尝试次数，包含首次尝试。小于 `1` 时按 `1` 次处理。 |
| `--rollback-on-failure` | 无 | 开启 | 安装或应用启动失败后，尝试从备份恢复旧版本。 |
| `--no-rollback-on-failure` | 无 | — | 关闭自动回滚。发生安装失败时保留备份并输出位置。 |
| `--payload-root` | 包内相对目录 | 自动识别 | 指定解压后的应用根目录。`.` 表示使用包根目录，`release` 表示使用包中的 `release` 目录。不能包含 `..`。 |
| `--updater-dir` | 顶层目录名 | `.updater` | 安装目录内的下载和解压工作目录。不能包含父目录路径，不能是 `.`。启动时该目录必须不存在。 |
| `--backup-dir-name` | 顶层目录名 | `.updater_backup` | 安装目录内的备份根目录。不能包含父目录路径，不能是 `.`，须与工作目录名称不同。需要安装时该目录必须不存在。 |

重复传入同一个带值参数时，最后一个值生效，`--keep` 会累计合并。启动和回滚的开启、关闭参数也按最后出现的设置生效。同时提供 `--local-package` 和在线更新参数时，使用本地包。

`--install-dir` 和 `--local-package` 的相对路径以启动更新器时的工作目录为基准。`--main-exe` 相对于安装目录，`--payload-root` 相对于解压目录。`--keep`、`--updater-dir` 和 `--backup-dir-name` 使用安装目录内的顶层名称。

在线更新可以在 PowerShell 中这样调用。示例中的版本、URL、安装目录和 PID 应替换为实际值。

```powershell
.\dist\windows-x64\updater.exe `
  --install-dir "G:\Apps\Demo" `
  --latest-json-url "https://updates.example.com/latest.json" `
  --current-version "1.2.0" `
  --main-exe "Demo.exe" `
  --keep "data,settings.json" `
  --pid 12345
```

本地安装可以关闭更新后的自动启动。该模式直接安装指定包，即使包版本较旧也会执行。

```powershell
.\dist\windows-x64\updater.exe `
  --install-dir "G:\Apps\Demo" `
  --local-package "G:\Downloads\Demo.zip" `
  --keep "data" `
  --no-start-main `
  --payload-root "."
```

macOS 和 Linux 使用相同参数，启动入口填写实际的可执行文件相对路径。

```sh
./updater \
  --install-dir "./demo-app" \
  --local-package "./demo.tar.zst" \
  --main-exe "bin/demo" \
  --keep "data" \
  --payload-root "."
```

`--keep` 只能指定顶层名称。要保留 `data/settings.json` 所在的数据目录，应传入 `--keep data`。下面两种写法效果相同。

```text
--keep "data,settings.json"
--keep "data" --keep "settings.json"
```

自动识别包根目录时，如果包顶层只有一个目录，更新器可能将其视为包装目录并使用其中的内容。如果启用自动启动，且入口文件已存在于包根目录下，则直接使用包根目录。可通过 `--payload-root .` 或 `--payload-root release` 明确指定布局。

```text
Demo.zip
└── release/
    ├── Demo.exe
    └── assets/
```

对于上面的包，可以传入 `--payload-root release --main-exe Demo.exe`。

`--main-args` 本身接收一个字符串。Windows 将字符串按原生命令行规则交给主程序。macOS 和 Linux 支持用空白分隔参数，并用双引号包住含空格的参数值，启动时不经过 shell。

下面的 PowerShell 写法将两个启动参数传给主程序，其中配置文件路径包含空格。该选项可追加到前面的更新命令中。

```powershell
--main-args '--config "G:\Apps\Demo\user settings.json" --minimized'
```

在 macOS 或 Linux 的 shell 中可以这样传入。

```sh
--main-args '--config "user settings.json" --minimized'
```

更新器执行完整版本替换。安装目录应只包含目标应用的文件，未通过 `--keep` 保留的旧内容会被替换。更新器可执行文件必须位于安装目录之外。更新包支持 ZIP 和 tar.zst 中的普通文件与目录，Unix 可执行文件需要在包中保留可执行权限。包含符号链接或硬链接的包会被拒绝。

运行期间，更新器使用安装目录内的 `.update-lock` 防止并发安装。正常退出会清理锁和本次工作目录。成功安装或成功回滚后会清理本次备份，回滚失败或关闭回滚时保留备份。进程被强制终止后，可能需要检查备份并手动恢复，再处理遗留的锁或目录。

调用方可以通过进程退出码判断结果。

| 退出码 | 含义 |
| --- | --- |
| `0` | 更新成功或无需更新，显示帮助也返回此值。 |
| `1` | 参数错误、清单错误、下载失败或其他致命错误，具体原因见日志。 |
| `2` | 更新包 URL 为空。 |
| `3` | 下载包的 SHA-256 与清单不一致。 |
| `10` | 在线模式缺少当前版本。 |
| `11` | 本地安装包不存在或不是普通文件。 |
| `20` | 更新失败，已成功回滚。 |
| `21` | 更新失败，回滚也失败，保留备份。 |
| `22` | 更新失败且已关闭自动回滚，保留备份。 |
