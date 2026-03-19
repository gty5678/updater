# updater.py (put in app root)
# Generic external updater:
# 1) read local version from CLI args (--current-version)
# 2) check remote latest.json
# 3) download package.zip
# 4) wait main process exit (by pid optional)
# 5) backup app dir
# 6) unpack and overlay app dir (keep some entries, default: data)
# 7) start main exe

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional
from urllib.error import URLError
from urllib.request import Request, urlopen


DEFAULT_LATEST_JSON_URL = (
    "https://raw.githubusercontent.com/de4321/darkeye/main/update/latest.json"
)


@dataclass(frozen=True)
class LatestInfo:
    latest_version: str
    release_notes: str
    package_url: str
    package_sha256: str


def _log(msg: str) -> None:
    print(f"[updater] {msg}", flush=True)


def _parse_version_tuple(v: str) -> Optional[tuple[int, ...]]:
    if not v:
        return None
    v = str(v).strip()
    if v.startswith(("v", "V")):
        v = v[1:]
    try:
        parts = [p for p in v.split(".") if p.strip() != ""]
        return tuple(int(p.strip()) for p in parts)
    except Exception:
        return None


def _is_newer(remote_version: str, local_version: str) -> bool:
    remote_t = _parse_version_tuple(remote_version)
    local_t = _parse_version_tuple(local_version)
    if remote_t is None or local_t is None:
        return (remote_version or "").strip() != (local_version or "").strip()
    return remote_t > local_t


def download_bytes(url: str, timeout_seconds: int = 20, retries: int = 3) -> bytes:
    # 网络环境不稳定时，HTTPS 握手可能被中断；做有限重试提升成功率。
    attempts = max(1, int(retries))
    last_err: Exception | None = None
    for i in range(attempts):
        try:
            req = Request(url, headers={"User-Agent": "DarkEye-Updater/1.0"})
            with urlopen(req, timeout=timeout_seconds) as resp:
                return resp.read()
        except (URLError, TimeoutError, OSError) as e:
            last_err = e
            if i >= attempts - 1:
                break
            sleep_s = min(8, 1 + i * 2)
            _log(
                f"Download failed (attempt {i + 1}/{attempts}): {e!r}; "
                f"retry in {sleep_s}s..."
            )
            time.sleep(sleep_s)
    raise RuntimeError(
        f"Download failed after {attempts} attempts: url={url} err={last_err!r}"
    )


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_latest_info(latest_json_url: str, timeout_seconds: int, retries: int) -> LatestInfo:
    raw = download_bytes(latest_json_url, timeout_seconds=timeout_seconds, retries=retries)
    data = json.loads(raw.decode("utf-8", errors="replace"))

    latest_version = str(data.get("latestVersion", "")).strip()
    release_notes = str(data.get("releaseNotes", "")).strip()

    pkg = data.get("package") or {}
    package_url = str(pkg.get("url", "")).strip()
    package_sha256 = str(pkg.get("sha256", "")).strip()

    if not latest_version or not package_url:
        raise RuntimeError("latest.json missing fields: latestVersion/package.url")

    return LatestInfo(
        latest_version=latest_version,
        release_notes=release_notes,
        package_url=package_url,
        package_sha256=package_sha256,
    )


def is_placeholder_sha256(s: str) -> bool:
    s = (s or "").strip().upper()
    return (not s) or "PUT_SHA256" in s or s == "0" * 64


def wait_for_pid_exit(pid: int, timeout_seconds: int) -> bool:
    # Windows-friendly approach without extra deps: poll using taskkill / tlist.
    # If pid check fails, we still can rely on timeout.
    end = time.time() + timeout_seconds
    while time.time() < end:
        try:
            # tasklist returns "INFO: No tasks are running..." or actual row.
            out = subprocess.check_output(
                ["tasklist", "/FI", f"PID eq {pid}"],
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="ignore",
            )
            if f"{pid}" not in out:
                return True
        except Exception:
            # If tasklist is unavailable, just break to avoid infinite loop.
            break
        time.sleep(1)
    return False


def ensure_empty_or_create(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True, exist_ok=True)


def copy_tree_overlay_with_keep(src_root: Path, dest_root: Path, keep_rel: set[str]) -> None:
    # 1) delete non-kept entries
    for item in dest_root.iterdir():
        rel = item.relative_to(dest_root).as_posix()
        if rel in keep_rel:
            continue
        # also keep hidden updater folders
        if rel.startswith(".updater/") or rel.startswith(".updater_backup/") or rel.startswith(".updater_backup"):
            continue
        # delete everything else
        if item.is_dir():
            shutil.rmtree(item, ignore_errors=True)
        else:
            try:
                item.unlink()
            except Exception:
                pass

    # 2) copy from extracted src_root into dest_root (overwrite)
    for item in src_root.iterdir():
        rel = item.relative_to(src_root).as_posix()
        # src_root should be a "version root" extracted, usually includes everything.
        if rel in keep_rel:
            continue

        dst = dest_root / item.name
        if item.is_dir():
            shutil.copytree(item, dst, dirs_exist_ok=True)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, dst)


def discover_extracted_payload(unpacked_dir: Path) -> Path:
    # zip may contain a top-level folder; detect it
    entries = [p for p in unpacked_dir.iterdir()]
    dirs = [p for p in entries if p.is_dir()]
    if len(dirs) == 1 and len(entries) == 1:
        return dirs[0]
    return unpacked_dir


def rollback_from_backup(backup_dir: Path, app_dir: Path, keep_rel: set[str]) -> None:
    _log(f"Rollback: restoring from backup: {backup_dir}")
    if not backup_dir.exists():
        raise FileNotFoundError(f"Backup dir not found: {backup_dir}")
    if not backup_dir.is_dir():
        raise NotADirectoryError(f"Backup path is not a directory: {backup_dir}")
    copy_tree_overlay_with_keep(src_root=backup_dir, dest_root=app_dir, keep_rel=keep_rel)
    _log("Rollback: completed.")


def safe_remove_tree(path: Path, label: str) -> None:
    if not path.exists():
        return
    try:
        shutil.rmtree(path, ignore_errors=False)
        _log(f"Cleanup removed {label}: {path}")
    except Exception as e:
        _log(f"Cleanup skipped {label} (remove failed): {e!r}")


def main() -> int:
    '''主要更新流程'''
    ap = argparse.ArgumentParser()
    #最新版本信息地址
    ap.add_argument("--latest-json-url", default=DEFAULT_LATEST_JSON_URL)
    #安装目录
    ap.add_argument("--install-dir", default=".")
    #主程序
    ap.add_argument("--main-exe", default="DarkEye.exe")
    #当前安装的版本
    ap.add_argument(
        "--current-version",
        default="",
        help="Current installed app version, e.g. 1.1.1 (required).",
    )
    #保留的目录，默认保留data目录
    ap.add_argument(
        "--keep",
        action="append",
        default=["data"],
        help="Top-level entries to keep (preserve and do not overwrite). "
        "Repeatable and supports comma-separated values. Default: data",
    )
    #失败时回滚，默认开启
    ap.add_argument(
        "--rollback-on-failure",
        action="store_true",
        default=True,
        help="Rollback to the pre-update backup if update fails (default: enabled).",
    )
    #失败时不回滚，默认关闭
    ap.add_argument(
        "--no-rollback-on-failure",
        dest="rollback_on_failure",
        action="store_false",
        help="Disable automatic rollback on update failure.",
    )
    #主程序进程id
    ap.add_argument("--pid", type=int, default=0)
    #等待主程序退出超时时间，默认120秒
    ap.add_argument("--wait-timeout-seconds", type=int, default=120)

    #下载超时时间，默认30秒
    ap.add_argument("--download-timeout-seconds", type=int, default=30)
    #下载重试次数，默认3次
    ap.add_argument("--download-retries", type=int, default=3)
    #包超时时间，默认120秒
    ap.add_argument("--package-timeout-seconds", type=int, default=120)
    #主程序参数
    ap.add_argument("--main-args", default="")
    #更新完成后是否自动启动主程序（默认开启）
    ap.add_argument(
        "--start-main",
        action="store_true",
        default=True,
        help="Start main executable after successful update (default: enabled).",
    )
    ap.add_argument(
        "--no-start-main",
        dest="start_main",
        action="store_false",
        help="Do not start main executable after successful update.",
    )
    #更新目录
    ap.add_argument("--updater-dir", default=".updater")
    #备份目录
    ap.add_argument("--backup-dir-name", default=".updater_backup")

    args = ap.parse_args()

    app_dir = Path(args.install_dir).resolve()
    updater_dir = app_dir / args.updater_dir
    backup_root = app_dir / args.backup_dir_name
    keep_user: set[str] = set()
    for raw in args.keep or []:
        for part in str(raw).split(","):
            name = part.strip().strip("/").strip("\\")
            if name:
                keep_user.add(name)

    _log(f"Working in: {app_dir}")

    # 1) 从参数读取本地版本
    local_version = str(args.current_version or "").strip()
    if not local_version:
        _log("Missing --current-version. Example: --current-version 1.1.1")
        return 10
    _log(f"Local version: {local_version}")

    # 2) 检查远端更新信息
    latest = load_latest_info(
        args.latest_json_url,
        timeout_seconds=args.download_timeout_seconds,
        retries=args.download_retries,
    )
    _log(f"Remote version: {latest.latest_version}")

    if not _is_newer(latest.latest_version, local_version):
        _log("No update needed.")
        return 0

    _log("Update is available. Preparing package download...")

    if not latest.package_url:
        _log("package_url is empty.")
        return 2

    # 3) 下载 update.zip
    # 下载目标：updater_dir/update.zip
    updater_dir.mkdir(parents=True, exist_ok=True)
    zip_path = updater_dir / "update.zip"
    packed = download_bytes(
        latest.package_url,
        timeout_seconds=args.package_timeout_seconds,
        retries=args.download_retries,
    )
    zip_path.write_bytes(packed)
    _log(f"Downloaded update package: {zip_path}")

    # 可选的 sha256 校验
    if not is_placeholder_sha256(latest.package_sha256):
        got = sha256_file(zip_path).lower()
        exp = latest.package_sha256.lower()
        if got != exp:
            _log(f"SHA256 mismatch. expected={exp} got={got}")
            return 3
        _log("SHA256 verified.")
    else:
        _log("SHA256 not provided or placeholder; skipping verification.")

    # 4) 等待主程序退出
    if args.pid and args.pid > 0:
        _log(f"Waiting for main PID {args.pid} to exit...")
        ok = wait_for_pid_exit(args.pid, timeout_seconds=args.wait_timeout_seconds)
        if not ok:
            _log("Main process still running after timeout. Continue anyway.")
    else:
        _log("No PID provided; waiting 3 seconds for manual shutdown...")
        time.sleep(3)

    # 5) 备份应用目录
    ts = time.strftime("%Y%m%d_%H%M%S")
    backup_dir = backup_root / f"backup_{ts}"
    _log(f"Backing up app to: {backup_dir}")

    backup_root.mkdir(parents=True, exist_ok=True)
    ensure_empty_or_create(backup_dir)

    # 复制整个 app_dir，但排除 updater 临时目录，避免递归拷贝
    def _ignore_backup(rel: str) -> bool:
        if rel.startswith(".updater/") or rel.startswith(".updater") or rel.startswith(".updater_backup/"):
            return True
        return False

    # shutil.copytree 的 ignore 机制更偏向名称匹配，这里直接遍历复制更直观
    for item in app_dir.iterdir():
        rel = item.name
        if _ignore_backup(rel):
            continue
        dst = backup_dir / item.name
        if item.is_dir():
            shutil.copytree(item, dst, dirs_exist_ok=True)
        else:
            shutil.copy2(item, dst)

    # 6) 解压 + 覆盖更新 + 启动（失败时可回滚）
    # 保留用户指定条目；同时始终保留 updater/backup 目录
    # 注意：主程序不再默认保留，确保会被更新包覆盖。
    keep_rel_update = set(keep_user)
    keep_rel_update.update({updater_dir.name, backup_root.name})
    # 回滚时不要保留 main exe，确保可由备份还原
    keep_rel_rollback = set(keep_user)
    keep_rel_rollback.update({updater_dir.name, backup_root.name})

    try:
        _log("Unpacking package...")
        unpacked_dir = updater_dir / "unpacked"
        ensure_empty_or_create(unpacked_dir)

        with zipfile.ZipFile(zip_path, "r") as zf:
            zf.extractall(unpacked_dir)

        payload_root = discover_extracted_payload(unpacked_dir)
        _log(f"Payload root: {payload_root}")

        _log(f"Overlay updating app/ (keep: {sorted(keep_user) if keep_user else []})...")
        copy_tree_overlay_with_keep(payload_root, app_dir, keep_rel=keep_rel_update)

        # 7) 按参数决定是否启动主程序
        if args.start_main:
            main_exe_path = app_dir / args.main_exe
            if not main_exe_path.exists():
                raise FileNotFoundError(f"Cannot find main exe after update: {main_exe_path}")

            main_args = args.main_args.strip()
            cmd = [str(main_exe_path)]
            if main_args:
                # 这里按空格简单拆分参数；复杂引号场景可后续增强
                cmd += main_args.split()

            _log(f"Starting main: {cmd}")
            subprocess.Popen(cmd, cwd=str(app_dir))
        else:
            _log("Skip starting main executable (--no-start-main).")

        # 成功更新后清理临时目录与备份目录
        safe_remove_tree(updater_dir, ".updater")
        safe_remove_tree(backup_root, ".updater_backup")

        _log("Updater finished.")

        return 0
    except Exception as e:
        _log(f"Update failed: {e!r}")
        if args.rollback_on_failure:
            try:
                rollback_from_backup(backup_dir=backup_dir, app_dir=app_dir, keep_rel=keep_rel_rollback)
                return 20
            except Exception as rb_e:
                _log(f"Rollback failed: {rb_e!r}")
                return 21
        return 22


if __name__ == "__main__":
    raise SystemExit(main())