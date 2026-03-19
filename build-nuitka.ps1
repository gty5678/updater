
# 清理旧的构建目录
Write-Host "Cleaning old build and dist folders..."

if (Test-Path build) {Remove-Item build -Recurse -Force}

if (Test-Path dist) {Remove-Item dist -Recurse -Force}

# 如果存在 __pycache__，也清掉
if (Test-Path __pycache__) {
    Remove-Item __pycache__ -Recurse -Force
}

# 1. 记录开始时间
$startTime = Get-Date

# 使用 Nuitka 打包
Write-Host "Building with Nuitka..."

$nuitkaArgs = @(
    "--msvc=latest",
    "--onefile",
    "--show-progress",
    "--show-memory",
    "--show-modules",
    "--lto=yes",#开启lto优化
    "--jobs=10",#开10个线程，不让电脑卡死
    # 打开控制台黑框便于调试
    #"--windows-console-mode=force",
    #"--windows-icon-from-ico=resources/icons/logo.ico",

    #"--report=report.xml",

    #后面是输出的地址和文件名
    "--output-dir=dist",
    "--output-filename=DarkEyeUpdater.exe"

    # updater.py 仅依赖标准库，显式排除常见第三方/GUI/科学计算包，避免误跟随
    "--nofollow-import-to=tkinter",
    "--nofollow-import-to=PyQt5",
    "--nofollow-import-to=PyQt6",
    "--nofollow-import-to=PySide2",
    "--nofollow-import-to=PySide6",
    "--nofollow-import-to=numpy",
    "--nofollow-import-to=pandas",
    "--nofollow-import-to=scipy",
    "--nofollow-import-to=matplotlib",
    "--nofollow-import-to=PIL",
    "--nofollow-import-to=cv2",
    "--nofollow-import-to=torch",
    "--nofollow-import-to=tensorflow",
    "--nofollow-import-to=sklearn",
        # 常见无关打包膨胀项
    "--nofollow-import-to=pytest",
    "--nofollow-import-to=test",
    "--nofollow-import-to=pydoc",
    "--nofollow-import-to=setuptools",
    "--nofollow-import-to=pkg_resources",
    "--nofollow-import-to=pip",
    "--nofollow-import-to=wheel",
    "--nofollow-import-to=virtualenv",
    "--nofollow-import-to=distutils",
    "--nofollow-import-to=venv",
    "--nofollow-import-to=doctest",
    "--nofollow-import-to=pdb",
    "--nofollow-import-to=trace",
    "--nofollow-import-to=tkinter"
)

nuitka @nuitkaArgs updater.py

#手动删除
#Remove-Item ".\dist\main.dist\PySide6\qml\QtMultimedia" -Recurse -Force -ErrorAction SilentlyContinue




Write-Host "Build complete."
# 3. 记录结束时间
$endTime = Get-Date

# 4. 计算耗时
$timeElapsed = $endTime - $startTime

# 5. 输出结果
Write-Host ("Elapsed time: {0} seconds" -f $timeElapsed.TotalSeconds)
