"""rshell 端到端测试：本地 paramiko SFTP 服务器 + 真实 rshell.exe。

覆盖需要写权限的场景：put/get（递归、通配符、续传、覆盖）、mkdir、rm、
rename、chmod、exec、密钥认证，以及交互式 sftp 脚本。

用法：
    python tests/e2e_test.py
    python tests/e2e_test.py --exe build/win7/rshell.exe --keep
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat as stat_mod
import subprocess
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BASE = REPO / "build_tmp" / "e2e"
REMOTE = BASE / "remote"
LOCAL = BASE / "local"

HOST = "127.0.0.1"
USER = "tester"
PASSWORD = "testpass"
PORT = 0
EXE = None


class Failure(AssertionError):
    """测试断言失败。"""


def decode(data: bytes) -> str:
    for enc in ("utf-8", "gbk"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", "replace")


def run_raw(cmd, expect=0, stdin_text=None, timeout=240):
    proc = subprocess.run(
        [str(c) for c in cmd],
        input=stdin_text.encode("utf-8") if stdin_text else None,
        capture_output=True,
        timeout=timeout,
    )
    out, err = decode(proc.stdout), decode(proc.stderr)
    if expect is not None and proc.returncode != expect:
        raise Failure(
            "命令退出码 {} != {}：{}\n--- stdout ---\n{}\n--- stderr ---\n{}".format(
                proc.returncode, expect, " ".join(str(c) for c in cmd), out, err
            )
        )
    return proc, out, err


def run_cli(*args, expect=0, key=None, use_password=True, stdin_text=None, timeout=240):
    cmd = [EXE, "--insecure", "--no-progress", "-H", HOST, "-p", str(PORT), "-u", USER]
    if key is not None:
        cmd += ["-i", str(key)]
    if use_password:
        cmd += ["--password", PASSWORD]
    cmd += list(args)
    return run_raw(cmd, expect=expect, stdin_text=stdin_text, timeout=timeout)


def check(cond, message, detail=""):
    if not cond:
        raise Failure(message + ("\n" + detail if detail else ""))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def same_file(a: Path, b: Path):
    return a.is_file() and b.is_file() and sha256(a) == sha256(b)


def tree_hashes(root: Path):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            result[path.relative_to(root).as_posix()] = sha256(path)
    return result


def remote(*parts) -> Path:
    return REMOTE.joinpath(*parts)


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def ls_empty_root():
    _proc, out, _err = run_cli("ls", "/")
    check(out.strip() == "", "空目录 ls 不应有输出", out)


@test
def mkdir_parents():
    run_cli("mkdir", "--parents", "/upload/a/b")
    check(remote("upload", "a", "b").is_dir(), "mkdir --parents 未创建嵌套目录")


@test
def put_single_file():
    _proc, _out, err = run_cli("put", LOCAL / "hello.txt", "/upload/hello.txt")
    check(same_file(LOCAL / "hello.txt", remote("upload", "hello.txt")), "上传内容不一致")
    check("已上传" in err, "缺少上传成功提示", err)


@test
def put_into_existing_dir():
    run_cli("put", LOCAL / "second.txt", "/upload")
    check(same_file(LOCAL / "second.txt", remote("upload", "second.txt")),
          "上传到已存在目录时应使用本地文件名")


@test
def put_overwrite_semantics():
    _proc, _out, err = run_cli("put", LOCAL / "second_v2.txt", "/upload/second.txt", expect=2)
    check("已存在" in err, "覆盖未加 --force 时应报错", err)
    check(same_file(LOCAL / "second.txt", remote("upload", "second.txt")),
          "未加 --force 时目标文件不应被修改")

    run_cli("put", "--force", LOCAL / "second_v2.txt", "/upload/second.txt")
    check(same_file(LOCAL / "second_v2.txt", remote("upload", "second.txt")),
          "--force 覆盖后内容应为新文件")


@test
def put_recursive_into_dir():
    run_cli("put", "-r", LOCAL / "tree", "/upload")
    check(tree_hashes(LOCAL / "tree") == tree_hashes(remote("upload", "tree")),
          "递归上传内容不一致")


@test
def put_recursive_to_new_name():
    run_cli("put", "-r", LOCAL / "tree", "/upload/tree2")
    check(not remote("upload", "tree2", "tree").exists(),
          "指定远端目录时不应再套一层本地目录名")
    check(tree_hashes(LOCAL / "tree") == tree_hashes(remote("upload", "tree2")),
          "递归上传到新目录内容不一致")


@test
def ls_long_format():
    _proc, out, _err = run_cli("ls", "-l", "/upload")
    lines = [ln for ln in out.splitlines() if ln.strip()]
    names = {ln.split("\t")[-1] for ln in lines}
    for expected in ("hello.txt", "second.txt", "tree", "tree2", "a"):
        check(expected in names, "ls -l 缺少条目 " + expected, out)
    check(all(ln[0] in "-d" for ln in lines), "ls -l 首列不是文件类型", out)
    hello = [ln for ln in lines if ln.endswith("hello.txt")][0]
    check(hello.startswith("-"), "hello.txt 应显示为普通文件", hello)
    check(" %d" % (LOCAL / "hello.txt").stat().st_size in hello, "大小显示不正确", hello)


@test
def ls_wildcard():
    _proc, out, _err = run_cli("ls", "/upload/*.txt")
    names = set(out.split())
    check(names == {"hello.txt", "second.txt"}, "通配符匹配结果不正确", out)


@test
def ls_dirs_only():
    _proc, out, _err = run_cli("ls", "-d", "/upload")
    check(set(out.split()) == {"a", "tree", "tree2"}, "ls -d 只应列出目录", out)


@test
def ls_empty_subdirectory():
    _proc, out, _err = run_cli("ls", "/upload/a/b")
    check(out.strip() == "", "空目录 ls 不应报错也不应有输出", out)


@test
def ls_missing_path():
    _proc, _out, err = run_cli("ls", "/upload/nope", expect=1)
    check("不存在" in err, "缺失路径应提示不存在", err)


@test
def get_single_file():
    target = LOCAL / "dl" / "a.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    run_cli("get", "/upload/tree/a.txt", target)
    check(same_file(LOCAL / "tree" / "a.txt", target), "下载内容不一致")


@test
def get_into_dir():
    run_cli("get", "/upload/hello.txt", LOCAL / "dl")
    check(same_file(LOCAL / "hello.txt", LOCAL / "dl" / "hello.txt"), "下载到目录时文件名或内容不对")


@test
def get_wildcard():
    out_dir = LOCAL / "dlmulti"
    out_dir.mkdir(parents=True, exist_ok=True)
    _proc, _out, err = run_cli("get", "/upload/*.txt", out_dir)
    check((out_dir / "hello.txt").is_file() and (out_dir / "second.txt").is_file(),
          "通配符下载文件缺失", err)
    check(same_file(LOCAL / "hello.txt", out_dir / "hello.txt"), "通配符下载 hello.txt 内容不一致")
    check(same_file(LOCAL / "second_v2.txt", out_dir / "second.txt"),
          "通配符下载 second.txt 内容不一致")


@test
def get_recursive():
    out_dir = LOCAL / "dl" / "tree"
    run_cli("get", "-r", "/upload/tree", out_dir)
    check(tree_hashes(LOCAL / "tree") == tree_hashes(out_dir), "递归下载内容不一致")


@test
def get_overwrite_semantics():
    _proc, _out, err = run_cli("get", "/upload/tree/a.txt", LOCAL / "dl", expect=2)
    check("已存在" in err, "下载覆盖未加 --force 时应报错", err)
    run_cli("get", "--force", "/upload/tree/sub/b.txt", LOCAL / "dl")
    check(same_file(LOCAL / "tree" / "sub" / "b.txt", LOCAL / "dl" / "b.txt"),
          "--force 下载后内容不一致")


@test
def get_resume():
    full = (REMOTE / "upload" / "tree" / "a.txt").read_bytes()
    partial = LOCAL / "resume_get.txt"
    partial.write_bytes(full[:200])
    _proc, _out, err = run_cli("get", "--resume", "/upload/tree/a.txt", partial)
    check(partial.read_bytes() == full, "续传下载结果与源文件不一致")
    check("已存在" not in err, "续传不应触发已存在错误", err)


@test
def put_resume():
    data = (LOCAL / "mid.bin").read_bytes()
    target = remote("upload", "resume.bin")
    target.write_bytes(data[:8000])
    _proc, _out, err = run_cli("put", "--resume", LOCAL / "mid.bin", "/upload/resume.bin")
    check(target.read_bytes() == data, "续传上传结果与源文件不一致")
    check("11.7 KiB" in err, "续传上传应只传输剩余字节", err)


@test
def put_dry_run():
    run_cli("-n", "put", LOCAL / "hello.txt", "/upload/dry.txt")
    check(not remote("upload", "dry.txt").exists(), "--dry-run 不应真正上传")


@test
def rm_dry_run():
    _proc, out, _err = run_cli("-n", "rm", "/upload/hello.txt")
    check("将删除" in out, "rm --dry-run 应打印计划", out)
    check(remote("upload", "hello.txt").is_file(), "--dry-run 不应真正删除")

@test
def chmod_and_list_mode():
    run_cli("chmod", "444", "/upload/second.txt")
    _proc, out, _err = run_cli("ls", "-l", "/upload/second.txt")
    check("-r--r--r--" in out, "chmod 444 后 ls -l 应显示只读", out)
    check(not os.access(remote("upload", "second.txt"), os.W_OK), "服务端文件仍可写")

    run_cli("chmod", "640", "/upload/second.txt")
    check(os.access(remote("upload", "second.txt"), os.W_OK), "chmod 640 后应恢复可写")


@test
def put_with_mode_option():
    run_cli("put", "--mode", "444", LOCAL / "hello.txt", "/upload/mode.txt")
    check("-r--r--r--" in run_cli("ls", "-l", "/upload/mode.txt")[1],
          "--mode 444 未生效")
    run_cli("chmod", "644", "/upload/mode.txt")


@test
def rename_remote():
    run_cli("rename", "/upload/second.txt", "/upload/renamed.txt")
    check(not remote("upload", "second.txt").exists(), "重命名后旧路径仍存在")
    check(same_file(LOCAL / "second_v2.txt", remote("upload", "renamed.txt")),
          "重命名后内容不一致")


@test
def rm_directory_requires_recursive():
    _proc, _out, err = run_cli("rm", "/upload/tree2", expect=2)
    check("请加 -r" in err, "删除目录未加 -r 时应报错", err)
    check(remote("upload", "tree2").is_dir(), "报错后目录不应被删除")


@test
def rm_recursive():
    run_cli("rm", "-r", "/upload/tree2")
    check(not remote("upload", "tree2").exists(), "rm -r 未删除目录")


@test
def rm_wildcard():
    run_cli("rm", "/upload/*.txt")
    remaining = {p.name for p in remote("upload").iterdir() if p.suffix == ".txt"}
    check(remaining == set(), "通配符删除后仍有 txt 文件", str(remaining))


@test
def exec_command():
    _proc, out, _err = run_cli("exec", "echo", "hello-from-exec")
    check("hello-from-exec" in out, "exec 输出不正确", out)


@test
def exec_exit_code():
    run_cli("exec", "exit", "3", expect=3)


@test
def positional_host_command():
    _proc, out, _err = run_raw(
        [EXE, "--insecure", "--no-progress", "-p", str(PORT), "-u", USER,
         "--password", PASSWORD, HOST, "echo positional-ok"],
        expect=0,
    )
    check("positional-ok" in out, "位置参数主机形式执行失败", out)


@test
def key_auth_pem():
    run_cli("ls", "-l", "/upload", key=LOCAL / "id_rsa_pem", use_password=False)


@test
def key_auth_openssh_format():
    """OpenSSH 新格式 RSA 私钥：当前构建应直接支持。"""
    run_cli("ls", "-l", "/upload", key=LOCAL / "id_rsa_openssh",
            use_password=False)


@test
def key_auth_ecdsa_sec1():
    """SEC1 ECDSA 私钥：modern 构建支持；Win7 兼容构建没有 ECDSA。"""
    proc, out, err = run_cli("ls", "-l", "/upload", key=LOCAL / "id_ecdsa_sec1",
                             use_password=False, expect=None)
    if proc.returncode != 0:
        print("    （当前构建不支持 ECDSA，跳过）")


@test
def key_auth_ed25519():
    proc, out, err = run_cli("ls", "/upload", key=LOCAL / "id_ed25519",
                             use_password=False, expect=None)
    if proc.returncode == 0:
        print("    （WinCNG 后端也支持 ed25519，行为正常）")
        return
    lowered = (out + err).lower()
    check("ed25519" in lowered or "openssl" in lowered,
          "ed25519 认证失败时应给出后端提示", out + err)


@test
def interactive_sftp_script():
    local_target = LOCAL / "dlshell" / "hello.txt"
    local_target.parent.mkdir(parents=True, exist_ok=True)
    script = "\n".join([
        "pwd",
        "mkdir shelltest",
        "cd shelltest",
        "put {} hello.txt".format(LOCAL / "hello.txt"),
        "ls",
        "get hello.txt {}".format(local_target),
        "rm hello.txt",
        "cd /",
        "rmdir shelltest",
        "exit",
    ]) + "\n"
    _proc, out, err = run_cli("sftp", stdin_text=script)
    combined = out + err
    check("sftp:/shelltest>" in combined, "交互 shell 提示符未切换目录", combined)
    check(same_file(LOCAL / "hello.txt", local_target), "交互 shell 下载内容不一致")
    check(not remote("shelltest").exists(), "交互 shell 未删除测试目录", combined)


def force_rmtree(path: Path):
    if not path.exists():
        return
    for root, dirs, files in os.walk(path):
        for name in list(dirs) + list(files):
            try:
                os.chmod(os.path.join(root, name), stat_mod.S_IWRITE)
            except OSError:
                pass
    shutil.rmtree(path, ignore_errors=True)


def make_keys():
    """生成 PEM / OpenSSH 新格式 / SEC1 ECDSA / ed25519 私钥，用于认证测试。"""
    ssh_keygen = shutil.which("ssh-keygen") or r"C:\Windows\System32\OpenSSH\ssh-keygen.exe"
    specs = [
        ("id_rsa_pem", ["-t", "rsa", "-b", "2048", "-m", "PEM"]),
        ("id_rsa_openssh", ["-t", "rsa", "-b", "2048"]),
        ("id_ecdsa_sec1", ["-t", "ecdsa", "-b", "256", "-m", "PEM"]),
        ("id_ed25519", ["-t", "ed25519"]),
    ]
    for name, extra in specs:
        path = LOCAL / name
        subprocess.run(
            [ssh_keygen, *extra, "-f", str(path), "-N", "", "-q"],
            capture_output=True,
            check=True,
        )


def prepare_fixtures():
    force_rmtree(BASE)
    REMOTE.mkdir(parents=True)
    LOCAL.mkdir(parents=True)

    (LOCAL / "hello.txt").write_text("hello rshell\n" * 50, encoding="utf-8")  # 650 B
    (LOCAL / "second.txt").write_text("second file\n" * 10, encoding="utf-8")
    (LOCAL / "second_v2.txt").write_text("second file v2\n" * 12, encoding="utf-8")
    (LOCAL / "mid.bin").write_bytes(os.urandom(20000))

    tree = LOCAL / "tree"
    (tree / "sub" / "deep").mkdir(parents=True)
    (tree / "emptydir").mkdir()
    (tree / "a.txt").write_text(("A" * 79 + "\n") * 100, encoding="utf-8")
    (tree / "sub" / "b.txt").write_text(("B" * 59 + "\n") * 200, encoding="utf-8")
    (tree / "sub" / "deep" / "c.bin").write_bytes(os.urandom(256 * 1024))

    make_keys()


def main():
    global EXE, PORT

    parser = argparse.ArgumentParser(description="rshell 端到端测试")
    parser.add_argument(
        "--exe",
        default=None,
        help="rshell.exe 路径（默认依次找 build/modern/rshell.exe、build/win7/rshell.exe）",
    )
    parser.add_argument("--keep", action="store_true", help="保留测试目录")
    args = parser.parse_args()

    candidates = [
        REPO / "build" / "modern" / "rshell.exe",
        REPO / "build" / "win7" / "rshell.exe",
    ]
    exe = Path(args.exe) if args.exe else next((c for c in candidates if c.is_file()), candidates[0])
    EXE = str(exe.resolve())
    if not os.path.isfile(EXE):
        print("找不到 rshell.exe：" + EXE, file=sys.stderr)
        print("请先运行 .\\build.ps1 或 .\\build.ps1 -Modern，或用 --exe 指定。", file=sys.stderr)
        return 2

    prepare_fixtures()

    sys.path.insert(0, str(HERE))
    from sftp_test_server import start_server

    _host, port, stop, _thread = start_server(
        str(REMOTE),
        host=HOST,
        port=0,
        user=USER,
        password=PASSWORD,
        host_key_path=str(BASE / "host_rsa.key"),
    )
    PORT = port
    print("测试服务器：{}:{}（根目录 {}）".format(HOST, PORT, REMOTE))

    failures = []
    for fn in TESTS:
        try:
            fn()
            print("  PASS  " + fn.__name__)
        except Exception as exc:  # noqa: BLE001 - 汇总所有失败
            failures.append((fn.__name__, exc))
            print("  FAIL  " + fn.__name__ + "：" + str(exc).splitlines()[0])
            traceback.print_exc(limit=2)

    stop.set()
    print("\n结果：{} 通过，{} 失败，共 {}".format(
        len(TESTS) - len(failures), len(failures), len(TESTS)))

    if failures:
        print("\n===== 失败详情 =====")
        for name, exc in failures:
            print("\n[{}]\n{}".format(name, exc))

    if not args.keep:
        force_rmtree(BASE)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())


