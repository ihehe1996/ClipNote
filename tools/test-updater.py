#!/usr/bin/env python
"""验证自更新换文件那一步（resources/apply-update.ps1）。

为什么需要这个工具：替换正在运行的程序自己这件事，**没法放进 --selftest**——
自检就跑在那个程序里，真换一次等于把自己删了。但它又是整个更新功能里唯一
「做错了会让用户手上只剩一个坏程序」的地方，不能不验。

所以这里的办法是：拿桩程序当替身，把同一个脚本（读的是 resources 下那一份，
不是抄一份）对着桩程序跑一遍。

两个桩程序是同一份 C 源码编出来的，靠宏区分：
  old  启动时在自身目录写 started-old.txt（里面有它的 pid），然后睡一会儿
  new  同上，写 started-new.txt

覆盖的场景：
  A 正常更新      → exe 被换成新的、新版本起来了、临时目录被清掉
  B 新版本起不来  → 回滚：老版本被还原**并重新启动**，临时目录留着（里面是日志）
  C 新版本不是个可执行文件 → 同样回滚

B、C 是重点。脚本要是漏了这两条路径，用户看到的现象是「更新完程序就打不开了」，
而且他几乎不可能想到该去看 %TEMP% 里的日志。

用法：
    python tools/test-updater.py [--keep]
退出码 0 = 全部通过。
"""
import argparse
import base64
import ctypes
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

GCC = r"D:\Program Files\QT\Tools\mingw1310_64\bin\gcc.exe"
TEMPLATE = os.path.join(ROOT, "resources", "apply-update.ps1")

# 绝对路径，不走 PATH —— 和产品代码里 UpdateInstaller::systemToolPath 一个道理
POWERSHELL = os.path.join(
    os.environ.get("SystemRoot", r"C:\Windows"),
    "System32", "WindowsPowerShell", "v1.0", "powershell.exe")

# 桩程序自己不能太快退出：脚本要「等它退出」才有意义
STUB_SOURCE = r"""
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef TAG
#define TAG "unknown"
#endif

/* 桩程序：启动时在自身目录留一个标记文件（里面带上自己的 pid，
   好让测试跑完能把还活着的进程收拾掉），然后按住 exe 不放。 */
int main(void)
{
    char path[MAX_PATH];
    char mark[MAX_PATH];
    FILE *f;
    char *slash;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';

    strcpy(mark, path);
    strcat(mark, "started-" TAG ".txt");
    f = fopen(mark, "w");
    if (f) { fprintf(f, "%s %lu\n", TAG, (unsigned long)GetCurrentProcessId()); fclose(f); }

#ifdef EXIT_IMMEDIATELY
    /* 模拟「新版本起不来」：起来就退 */
    return 3;
#else
    Sleep(120000);
#endif
    return 0;
}
"""


def build_stub(workdir, tag, exit_immediately=False):
    src = os.path.join(workdir, f"stub-{tag}.c")
    exe = os.path.join(workdir, f"stub-{tag}.exe")
    with open(src, "w", encoding="utf-8") as handle:
        handle.write(STUB_SOURCE)

    cmd = [GCC, src, "-o", exe, f'-DTAG="{tag}"']
    if exit_immediately:
        cmd.append("-DEXIT_IMMEDIATELY")
    subprocess.run(cmd, check=True, capture_output=True)
    return exe


def render_script(exe, stage, work, pid):
    """和 UpdateInstaller::launchSwap 做同样的替换，**读的是同一份模板**。"""
    with open(TEMPLATE, "r", encoding="utf-8") as handle:
        text = handle.read()

    quote = lambda s: s.replace("'", "''")          # noqa: E731
    text = text.replace("@@EXE@@", quote(exe))
    text = text.replace("@@STAGE@@", quote(stage))
    text = text.replace("@@WORK@@", quote(work))
    text = text.replace("@@PID@@", str(pid))
    return text


def launch_swap(exe, stage, work, pid):
    script = render_script(exe, stage, work, pid)
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    return subprocess.Popen(
        [POWERSHELL, "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
        creationflags=subprocess.CREATE_NO_WINDOW)


def kill(pid):
    subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                   capture_output=True, check=False)


def wait_for(path, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def read_pid(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return int(handle.read().split()[1])
    except (OSError, IndexError, ValueError):
        return None


def scenario(name, new_exe_builder, expect_success, sandbox):
    """跑一个场景。new_exe_builder(stage_dir) 负责把「新版本」摆进去。"""
    print(f"\n=== {name} ===")

    case = os.path.join(sandbox, name)
    shutil.rmtree(case, ignore_errors=True)
    install = os.path.join(case, "install")     # 假装是用户装 ClipNote 的目录
    stage = os.path.join(case, "stage")
    work = os.path.join(case, "work")
    for path in (install, stage, work):
        os.makedirs(path)

    old_exe = build_stub(case, "old")
    target = os.path.join(install, "ClipNote.exe")
    shutil.copy2(old_exe, target)
    with open(old_exe, "rb") as handle:
        old_bytes = handle.read()

    new_exe_builder(stage)

    # 把「旧版本」跑起来 —— 它占着 exe，这才是真实处境
    proc = subprocess.Popen([target], creationflags=subprocess.CREATE_NO_WINDOW)
    old_mark = os.path.join(install, "started-old.txt")
    if not wait_for(old_mark):
        proc.kill()
        return [("旧版本没能启动（测试自身的问题）", False)]

    # 清掉刚才那个标记：回滚路径要靠它的**重新出现**来判断老版本被重新启动了
    os.remove(old_mark)

    launcher = launch_swap(target, stage, work, proc.pid)

    # 真正的 ClipNote 在启动脚本之后会立刻退出，这里照做
    time.sleep(1.5)
    kill(proc.pid)

    try:
        launcher.wait(timeout=120)
    except subprocess.TimeoutExpired:
        launcher.kill()
        return [("脚本 120 秒没跑完", False)]

    with open(target, "rb") as handle:
        now_bytes = handle.read()
    with open(os.path.join(stage, "ClipNote.exe"), "rb") as handle:
        new_bytes = handle.read()

    log_path = os.path.join(work, "apply-update.log")
    log = ""
    if os.path.exists(log_path):
        with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
            log = handle.read()

    results = []

    if expect_success:
        results.append(("exe 被换成了新版本", now_bytes == new_bytes))
        results.append(("新版本确实跑起来了",
                        wait_for(os.path.join(install, "started-new.txt"), 10)))
        results.append(("临时目录被清掉（没有留下垃圾）", not os.path.exists(work)))
        results.append(("脚本退出码为 0", launcher.returncode == 0))
    else:
        results.append(("exe 被还原成原来那一版", now_bytes == old_bytes))
        results.append(("老版本被重新启动了",
                        wait_for(os.path.join(install, "started-old.txt"), 10)))
        results.append(("临时目录被留着（里面是日志和备份）", os.path.exists(work)))
        results.append(("脚本退出码非 0，调用方能看出失败", launcher.returncode != 0))
        results.append(("日志里记了回滚", "回滚" in log))

    # 收拾还活着的桩程序，否则沙箱删不掉
    for marker in ("started-old.txt", "started-new.txt"):
        pid = read_pid(os.path.join(install, marker))
        if pid:
            kill(pid)

    for desc, ok in results:
        print(f"  [{'OK ' if ok else 'FAIL'}] {desc}")
    if not all(ok for _, ok in results) and log:
        print("  ---- apply-update.log ----")
        for line in log.strip().splitlines():
            print(f"  | {line}")

    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keep", action="store_true", help="跑完不删沙箱，留着排查")
    args = parser.parse_args()

    if not os.path.exists(GCC):
        print(f"找不到编译器：{GCC}")
        return 2
    if not os.path.exists(POWERSHELL):
        print(f"找不到 PowerShell：{POWERSHELL}")
        return 2

    sandbox = os.path.join(os.environ.get("TEMP", "/tmp"), "ClipNote-updater-test")
    shutil.rmtree(sandbox, ignore_errors=True)
    os.makedirs(sandbox)
    print(f"沙箱：{sandbox}")

    def stage_new_ok(stage):
        shutil.copy2(build_stub(os.path.dirname(stage), "new"), os.path.join(stage, "ClipNote.exe"))
        # 顺带一个依赖 DLL，验证「不只换 exe」这件事
        with open(os.path.join(stage, "Qt6Fake.dll"), "wb") as handle:
            handle.write(b"MZ" + b"\0" * 64)

    def stage_new_crashes(stage):
        exe = build_stub(os.path.dirname(stage), "crash", exit_immediately=True)
        shutil.copy2(exe, os.path.join(stage, "ClipNote.exe"))

    def stage_new_garbage(stage):
        # 根本不是可执行文件。Start-Process 会直接抛异常 ——
        # 这条路径以前是漏掉回滚的，专门钉一条。
        with open(os.path.join(stage, "ClipNote.exe"), "wb") as handle:
            handle.write(b"<html>404 not found</html>")

    all_results = []
    all_results += scenario("A-正常更新", stage_new_ok, True, sandbox)
    all_results += scenario("B-新版本起不来", stage_new_crashes, False, sandbox)
    all_results += scenario("C-新版本不是可执行文件", stage_new_garbage, False, sandbox)

    passed = sum(1 for _, ok in all_results if ok)
    total = len(all_results)
    print(f"\n{'=' * 40}\n{passed}/{total} 项通过")

    if args.keep:
        print(f"沙箱留在 {sandbox}")
    else:
        shutil.rmtree(sandbox, ignore_errors=True)

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
