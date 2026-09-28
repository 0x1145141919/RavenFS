#!/usr/bin/env python3
"""从 kbuild 的 .cmd 文件生成 linux/compile_commands.json，喂给 VSCode cpptools / clangd。

原理: kbuild 每次 make 都会在 linux/ 下写 `.xxx.o.cmd`，里面第一行
`savedcmd_<obj> := gcc ... -c -o <obj> <src>` 就是真实编译命令，
连 -I 内核头路径、-include kconfig.h、-D__KERNEL__ 全都有——直接提取即可。

用法:
    python3 tools/ide/gen_compile_commands.py
    （若经 `make -C linux` 顶层 all 目标构建，构建后会自动跑一次）

重新生成时机:
    - linux/ 下新增/删除 .c 文件后
    - 内核升级后（.cmd 里是旧版本路径，需先重新 make）
    - 换 KDIR/内核版本重新 make 后

userspace 侧 (mkfs/tests/juvenile_god) 不归本脚本管：CMake 的
build/compile_commands.json 由 cmake 每次构建自动刷新，两者在
.vscode/c_cpp_properties.json 里以数组形式并列引用。
"""
import json
import os
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINUX = ROOT / "linux"
OUT = LINUX / "compile_commands.json"

SAVEDCMD_RE = re.compile(r"^savedcmd_(\S+) := (.*)$", re.M)
SOURCE_RE = re.compile(r"^source_(\S+) := (.*)$", re.M)
KVER_RE = re.compile(r"-I(/usr/lib/modules/([^/]+)/)")


def kernel_entries():
    entries = []
    seen = set()
    for cmdfile in sorted(LINUX.glob(".*.cmd")):
        text = cmdfile.read_text(errors="replace")
        mc = SAVEDCMD_RE.search(text)
        ms = SOURCE_RE.search(text)
        if not mc or not ms or mc.group(1) != ms.group(1):
            continue
        src = ms.group(2).strip()
        # 只要真正的编译条目（排除 ld 链接 / printf 生成 ravenfs.mod / objtool 串）
        if not src.endswith((".c", ".S")):
            continue
        raw = mc.group(2)
        if " ; " in raw:  # kbuild 链式命令（gcc ... ; objtool ...）只取编译段
            raw = raw.split(" ; ", 1)[0]
        try:
            args = shlex.split(raw)
        except ValueError as e:
            print(f"  [skip] {cmdfile.name}: shlex 失败 {e}", file=sys.stderr)
            continue
        if "-c" not in args or not args or not args[-1].endswith((".c", ".S")):
            continue
        src_path = Path(src)
        if not src_path.is_absolute():
            src_path = LINUX / src_path
        src_path = src_path.resolve()
        # 只收录仓库内源文件（module-common.c 之类内核自带文件不用管）
        if not str(src_path).startswith(str(ROOT)) or not src_path.exists():
            continue
        if str(src_path) in seen:
            continue
        seen.add(str(src_path))
        entries.append(
            {
                "directory": str(LINUX),
                "file": str(src_path),
                # cpptools/clangd 两者皆认：arguments 数组为准，command 供 wordexp 路径用
                "arguments": args,
                "command": shlex.join(args),
            }
        )
    return entries


def check_kernel_freshness(entries):
    """cmd 里的内核版本和当前运行内核不一致时提醒（升级内核后未重编）。"""
    for e in entries:
        m = KVER_RE.search(e["command"])
        if m and m.group(2) != os.uname().release:
            print(
                f"  [warn] .cmd 用的是内核 {m.group(2)}，当前 {os.uname().release}；"
                "路径已过期，先重新 make 再跑本脚本",
                file=sys.stderr,
            )
        return


def main():
    if not LINUX.is_dir():
        sys.exit(f"no linux/ dir at {LINUX}")
    entries = kernel_entries()
    if not entries:
        sys.exit("没提取到任何条目——linux/ 下没有 .cmd？先 make -C linux 编一次")
    OUT.write_text(json.dumps(entries, indent=2, ensure_ascii=False) + "\n")
    check_kernel_freshness(entries)
    print(f"[gen] {OUT}  ({len(entries)} entries)")
    for e in entries:
        print(f"      {Path(e['file']).name}")


if __name__ == "__main__":
    main()
