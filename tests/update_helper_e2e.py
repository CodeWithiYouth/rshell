"""Exercise the actual updater on disposable executable copies only."""
import argparse
import ctypes
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes


def dismiss_helper_dialogs(process, timeout=10):
    user32 = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    deadline = time.monotonic() + timeout
    while process.poll() is None and time.monotonic() < deadline:
        @callback_type
        def close_owned_window(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == process.pid and user32.IsWindowVisible(hwnd):
                user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
            return True
        user32.EnumWindows(close_owned_window, 0)
        time.sleep(0.05)
    process.communicate(timeout=3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--gui', required=True)
    parser.add_argument('--fixture', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / 'build_tmp'
    root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='更新 测试 ', dir=root) as temporary:
        directory = Path(temporary)
        target = directory / 'rshell gui.exe'
        staged = directory / 'rsu1234.tmp'
        helper_exe = directory / 'helper.tmp'
        shutil.copyfile(args.gui, helper_exe)
        target.write_bytes(b'original-placeholder')
        shutil.copyfile(args.fixture, staged)
        expected = hashlib.sha256(staged.read_bytes()).hexdigest()
        # 当前测试进程真实存在：助手必须等待它退出，不能提前替换。
        parent = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(1.5)'])
        helper = subprocess.Popen([str(helper_exe), '--rshell-update-apply', str(parent.pid), str(target), str(staged)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.3)
        assert target.read_bytes() == b'original-placeholder', 'updated before original process exited'
        parent.wait(timeout=10)
        helper.communicate(timeout=20)
        result = helper
        if result.returncode:
            raise AssertionError(f'updater failed: {result.returncode}')
        deadline = time.monotonic() + 10
        marker = Path(str(target) + '.started')
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert marker.exists(), 'replacement executable was not started'
        assert hashlib.sha256(target.read_bytes()).hexdigest() == expected
        assert not staged.exists(), 'staged executable was not consumed'
        assert not list(directory.glob('rsb*.tmp')), 'successful update left a backup'
        # 参数非法时退出，不接触目标。
        result = subprocess.run([args.gui, '--rshell-update-apply', 'bad', str(target), str(staged)],
                                timeout=10, capture_output=True)
        assert result.returncode == 2
        assert hashlib.sha256(target.read_bytes()).hexdigest() == expected
        # 新文件启动失败时，助手必须把已备份的原文件换回来。
        failed_target = directory / 'restore target.exe'
        failed_staged = directory / 'rsu5555.tmp'
        failed_target.write_bytes(b'original-for-rollback')
        failed_staged.write_bytes(b'not-an-executable')
        failed = subprocess.Popen([str(helper_exe), '--rshell-update-apply', str(parent.pid),
                                   str(failed_target), str(failed_staged)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        dismiss_helper_dialogs(failed)
        assert failed.returncode == 5, f'expected launch failure, got {failed.returncode}'
        assert failed_target.read_bytes() == b'original-for-rollback', 'original was not restored'
        assert not list(directory.glob('rsb*.tmp')), 'rollback left a backup'
        # 另一个实例占用文件时必须停止替换并保留原内容。
        locked_target = directory / 'locked target.exe'
        locked_staged = directory / 'rsu6666.tmp'
        locked_target.write_bytes(b'original-locked')
        shutil.copyfile(args.fixture, locked_staged)
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateFileW.restype = wintypes.HANDLE
        handle = kernel32.CreateFileW(str(locked_target), 0x80000000, 0, None, 3, 0, None)
        assert handle != wintypes.HANDLE(-1).value, 'could not lock test target'
        try:
            locked = subprocess.Popen([str(helper_exe), '--rshell-update-apply', str(parent.pid),
                                        str(locked_target), str(locked_staged)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            dismiss_helper_dialogs(locked)
            assert locked.returncode == 4, f'expected replace failure, got {locked.returncode}'
        finally:
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle(handle)
        assert locked_target.read_bytes() == b'original-locked', 'locked original was changed'
        assert locked_staged.exists(), 'failed replacement should preserve the staged file'
    print('PASS: temporary replacement, restart, rollback, locked target and invalid arguments')


if __name__ == '__main__':
    main()
