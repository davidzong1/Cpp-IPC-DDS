#!/usr/bin/env python3
# ============================================================================
# dzIPC Script Manager — cross-platform TUI for managing scripts/
#
# Usage:
#   python manage.py              Launch interactive menu
#   python manage.py --list       List available scripts and exit
#   python manage.py --help       Show help
#
# Detects OS automatically and shows only the appropriate scripts
# (.bat on Windows, .sh on Linux / macOS).
# ============================================================================
from __future__ import annotations
import os
import sys
import shlex
import subprocess
import platform
import shutil
from pathlib import Path
import subprocess
import ctypes

# ---- Project paths ----
MANAGER_DIR = Path(__file__).resolve().parent
FILE_PATH = Path(__file__).resolve()
SCRIPTS_DIR = MANAGER_DIR / "scripts"
IS_WINDOWS = platform.system() == "Windows"

if IS_WINDOWS:
    import winreg

# ---- Script registry: name → {desc, mode} ----
# mode: "script" = a scripts/<name><ext> entry, executed directly
SCRIPT_REGISTRY = [
    {
        "name": "install",
        "desc": "Full installation wizard (C++ / Python / msg-srv / dispatcher)",
        "mode": "script",
    },
    {
        "name": "setup",
        "desc": "Activate dzIPC environment in current shell",
        "mode": "script",
    },
    {
        "name": "update_msg_srv",
        "desc": "Sync & regenerate msg/srv files from external paths",
        "mode": "script",
    },
]

# For native script matching: scripts named as <base>.<ext>
EXT = ".bat" if IS_WINDOWS else ".sh"

# ---- Terminal helpers ----


def term_width():
    return shutil.get_terminal_size().columns


def clear():
    if IS_WINDOWS:
        os.system("cls")
    else:
        os.system("clear")


# ---- Color helpers (ANSI escape, works in modern Win10+ too) ----


def _esc(code):
    return f"\033[{code}m"


BOLD = _esc(1)
DIM = _esc(2)
RED = _esc(31)
GREEN = _esc(32)
YELLOW = _esc(33)
BLUE = _esc(34)
MAGENTA = _esc(35)
CYAN = _esc(36)
NC = _esc(0)


def with_tag(text, color):
    """Wrap text with a color tag, e.g. '[script]' in green."""
    return f"{color}{text}{NC}"


# ---- Box drawing helpers ----

H_LINE = "═"
V_LINE = "║"
TL = "╔"
TR = "╗"
BL = "╚"
BR = "╝"
T_JUNC = "╦"
B_JUNC = "╩"
V_RIGHT = "╠"
V_LEFT = "╣"
CROSS = "╬"
H_DIV = "─" * 52


def draw_box_top():
    return f"{CYAN}{BOLD}{TL}{H_LINE * 52}{TR}{NC}"


def draw_box_bottom():
    return f"{CYAN}{BOLD}{BL}{H_LINE * 52}{BR}{NC}"


def draw_box_line(text):
    """Draw a single line inside the box."""
    # 估算显示宽度：ASCII=1，CJK=2
    w = sum(2 if ord(c) > 0x2E80 else 1 for c in text)
    pad = max(0, 52 - w - 2)   # 关键：减去行首和行尾的两个空格，不然显示错误
    return f"{CYAN}{BOLD}{V_LINE}{NC} {text}{' ' * pad} {CYAN}{BOLD}{V_LINE}{NC}"


def draw_box_sep():
    return f"{CYAN}{BOLD}{V_RIGHT}{H_LINE * 52}{V_LEFT}{NC}"


# ---- Header / footer ----


def print_header():
    clear()
    print(draw_box_top())
    print(draw_box_line("dzIPC Script Manager"))
    print(draw_box_bottom())
    print()
    print(f"  {DIM}OS:          {NC}{platform.system()} {platform.release()}")
    print(f"  {DIM}Project root:{NC} {MANAGER_DIR}")
    print(f"  {DIM}Scripts path:{NC} {SCRIPTS_DIR}")
    print()


def print_footer(count):
    print()
    print(f"{DIM}{H_DIV}{NC}")
    print(
        f"{DIM}  q) Quit    h) Help    l) List    r) Refresh"
        + f"    [1-{count}]{DIM} select{NC}"
    )
    print(
        f"{DIM}  Tip: append run arguments after the number, e.g. "
        f"{NC}{GREEN}{count} --topic foo -v{NC}"
    )
    print()


# ---- Script list helpers ----


def available_scripts():
    """Return list of (name, ext, mode, desc) for scripts that exist."""
    result = []
    for entry in SCRIPT_REGISTRY:
        script_file = SCRIPTS_DIR / f"{entry['name']}{EXT}"
        if script_file.is_file():
            result.append((entry["name"], EXT, entry["mode"], entry["desc"]))
    return result


def mode_tag(mode):
    if mode == "script":
        return with_tag("[script]", GREEN)
    elif mode == "exec":
        return with_tag(f"[exec]", BLUE)
    elif mode == "python":
        return with_tag(f"[py]", CYAN)
    return with_tag("[???]", RED)


def print_scripts(scripts):
    for i, (name, ext, mode, desc) in enumerate(scripts, 1):
        tag = mode_tag(mode)
        filename = f"{name}{ext}"
        print(f"  {BOLD}{i:>2}){NC} {tag} {filename:<28s} {DIM}— {desc}{NC}")
    print()


# ---- Help ----


def print_help():
    print()
    print(f"{YELLOW}{BOLD}Help{NC}")
    print()
    print(f"  {GREEN}[script]{NC}  Script is executed directly from project root.")
    print()
    if IS_WINDOWS:
        print(f"  {BOLD}Tip:{NC} Run {GREEN}scripts\\setup.bat{NC} to activate the")
        print(f"       dzIPC environment in your current cmd session.")
    else:
        print(f"  {BOLD}Tip:{NC} Run {GREEN}source scripts/setup.sh{NC} to activate the")
        print(f"       dzIPC environment in your current shell.")
    print()
    print(
        f"  {BOLD}Tip:{NC} Use {GREEN}python manage.py --list{NC} for a quick overview."
    )
    print()
    input("Press Enter to return to menu...")


# ---- Run / execute a script ----

# Scripts that refuse direct execution and must be SOURCED, so the environment
# they export survives in the shell the user is left with. Selecting one opens
# a new shell with it already sourced, on Windows as well.
SOURCED_SCRIPTS = {"setup"}


def run_sourced(name, ext, script_path, args):
    """Open a new shell with `script_path` sourced into it.

    Neither `bash script.sh` nor `cmd /c script.bat` can work here: exported
    variables die with the child process. We hand the terminal over to a shell
    that sources the script first, so the environment is usable interactively.
    """
    print()
    if args:
        print(
            f"{YELLOW}Note: '{name}{ext}' is a setup script — "
            f"ignoring arguments: {shlex.join(args)}{NC}"
        )
        print()

    if IS_WINDOWS:
        # `call` keeps the batch file in the current cmd session, then /k keeps
        # cmd open so the activated environment is actually usable.
        print(
            f"{YELLOW}{BOLD}  {name}{ext} must be CALLED, not spawned standalone.{NC}"
        )
        print()
        print(
            f"{BOLD}  Launching a new cmd with dzIPC environment activated...{NC}"
        )
        print(f"  {DIM}(Type 'exit' to return to your original session.){NC}")
        print()
        # Replace this Python process with the new cmd — same reasoning as the
        # bash branch below: no parent process is left holding the terminal.
        os.execvp(
            "cmd",
            [
                "cmd",
                "/k",
                f'call "{script_path}"',
            ],
        )
    else:
        print(f"{YELLOW}{BOLD}  {name}{ext} must be SOURCED, not executed.{NC}")
        print()
        print(
            f"{BOLD}  Launching a new shell with dzIPC environment activated...{NC}"
        )
        print(f"  {DIM}(Type 'exit' to return to your original shell.){NC}")
        print()
        # Replace the current Python process with a new bash that sources the
        # script, then drops into an interactive shell. Environment vars set by
        # the script persist because we use exec, not spawn — the bash replaces
        # this TUI process in-place.
        os.execvp(
            "bash",
            [
                "bash",
                "-c",
                f"source '{script_path}' && exec bash -i",
            ],
        )


def run_script(name, ext, mode, args=None):
    """Run one entry produced by the scanners.

    mode "script" -> scripts/<name><ext>   executed directly
    mode "python" -> tools/<name>/main.py  executed with the current interpreter
    mode "exec"   -> build/app/<name>      built executable, executed directly

    Names in SOURCED_SCRIPTS are the exception: they are sourced into a new
    shell instead of being executed (see run_sourced).

    `args` are extra command-line arguments appended after the target, exactly
    as typed on the selection line (e.g. `4 --topic foo -v`).
    """
    args = list(args) if args else []

    if mode == "python":
        script_path = MANAGER_DIR / "tools" / name / "main.py"
    elif mode == "exec":
        script_path = MANAGER_DIR / "build" / "app" / name
    else:
        script_path = SCRIPTS_DIR / f"{name}{ext}"

    # ---- sourced scripts: hand over to a shell instead of executing ----
    if name in SOURCED_SCRIPTS:
        if not script_path.exists():
            print()
            print(f"{RED}{BOLD}[FAIL]{NC} Not found: {script_path}")
            print()
            input("Press Enter to return to menu...")
            return
        try:
            run_sourced(name, ext, script_path, args)
        except OSError as error:
            print(f"{RED}{BOLD}[FAIL]{NC} Cannot launch shell: {error}")
            print()
            input("Press Enter to return to menu...")
        return

    # ---- direct execution: pick argv + the command line to display ----
    if mode == "python":
        interpreter = sys.executable or ("python" if IS_WINDOWS else "python3")
        argv = [interpreter, str(script_path)]
        shown = f"{interpreter} tools/{name}/main.py"
    elif mode == "exec":
        argv = [str(script_path)]
        shown = f"build/app/{name}"
    elif IS_WINDOWS:
        argv = ["cmd", "/c", str(script_path)]
        shown = f"scripts\\{name}{ext}"
    else:
        argv = ["bash", str(script_path)]
        shown = f"bash scripts/{name}{ext}"

    argv += args
    if args:
        shown = f"{shown} {shlex.join(args)}"

    print()
    print(f"{GREEN}{BOLD}> Running:{NC} {shown}")
    print(f"{DIM}{H_DIV}{NC}")
    print()

    if not script_path.exists():
        print(f"{RED}{BOLD}[FAIL]{NC} Not found: {script_path}")
        print()
        input("Press Enter to return to menu...")
        return

    try:
        result = subprocess.run(argv, cwd=str(MANAGER_DIR))
    except KeyboardInterrupt:
        print()
        print(f"{YELLOW}Script interrupted by user.{NC}")
        print()
        input("Press Enter to return to menu...")
        return
    except OSError as error:
        print(f"{RED}{BOLD}[FAIL]{NC} Cannot run {shown}: {error}")
        print()
        input("Press Enter to return to menu...")
        return

    print()
    if result.returncode == 0:
        print(f"{GREEN}{BOLD}[OK]{NC} Script completed successfully.")
    else:
        print(f"{RED}{BOLD}[FAIL]{NC} Script exited with code {result.returncode}.")
    print()
    input("Press Enter to return to menu...")


# ---- Main loop ----


def main_loop(scripts):
    while True:
        if not scripts:
            clear()
            print(f"{RED}No scripts found in {SCRIPTS_DIR}{NC}")
            print()
            print("Press Ctrl+C to exit.")
            try:
                input()
            except KeyboardInterrupt:
                print()
                print(f"{GREEN}Goodbye!{NC}")
                sys.exit(0)
            continue

        print_header()
        print_scripts(scripts)
        print_footer(len(scripts))

        try:
            choice = input(f"  Select [1-{len(scripts)}/q/h/l/r]: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            print(f"{GREEN}Goodbye!{NC}")
            sys.exit(0)

        # Split the line into the command token and any run arguments, e.g.
        # "4 --topic foo -v" -> ("4", ["--topic", "foo", "-v"]).
        # shlex handles quoted values ("4 --topic 'my topic'") and falls back
        # to a plain split when the quotes are unbalanced.
        try:
            tokens = shlex.split(choice)
        except ValueError:
            tokens = choice.split()

        if not tokens:
            continue

        cmd, run_args = tokens[0], tokens[1:]

        if cmd in ("q", "Q", "quit", "exit"):
            print()
            print(f"{GREEN}Goodbye!{NC}")
            sys.exit(0)
        elif cmd in ("h", "H", "help"):
            print_help()
        elif cmd in ("l", "L", "list"):
            print_header()
            print_scripts(scripts)
            input("Press Enter to return to menu...")
        elif cmd in ("r", "R", "refresh"):
            continue
        elif cmd.isdigit():
            idx = int(cmd)
            if 1 <= idx <= len(scripts):
                run_script(*scripts[idx - 1][:3], args=run_args)
            else:
                print(f"{RED}Invalid selection: '{choice}'{NC}")
                import time

                time.sleep(1)
        else:
            print(f"{RED}Invalid selection: '{choice}'{NC}")
            import time

            time.sleep(1)


def scan_tool_dir():
    """Scan the tools directory and return a list of available tools.

    Only sub-directories containing a `main.py` file are treated as tools.

    Returns entries in the same (name, ext, mode, desc) shape as
    available_scripts(); the tool is identified by its folder name, so `ext`
    is empty and the displayed name is just `<tool>`.
    """
    tools = []
    tool_dir_path: str = MANAGER_DIR / "tools"

    for item in os.listdir(tool_dir_path):
        item_path = os.path.join(tool_dir_path, item)

        # Only consider sub-directories
        if not os.path.isdir(item_path):
            continue

        main_py_path = os.path.join(item_path, "main.py")

        # The sub-directory must contain a main.py
        if not os.path.isfile(main_py_path):
            continue

        # Avoid duplicates
        if any(tool[0] == item for tool in tools):
            continue

        tools.append((item, "", "python", f"Python tool {item} script"))

    return tools


def _is_in_path(bin_dir: Path) -> bool:
    """判断 bin_dir 是否已在当前进程 PATH 中。"""
    try:
        current = bin_dir.resolve()
    except OSError:
        current = bin_dir
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if not entry:
            continue
        try:
            if Path(entry).resolve() == current:
                return True
        except OSError:
            continue
    return False


def _add_to_path_unix(bin_dir: Path) -> None:
    """在 shell rc 文件中追加 PATH 导出（幂等）。"""
    export_line = f'\nexport PATH="{bin_dir}:$PATH"\n'
    shell = os.environ.get("SHELL", "")

    if "zsh" in shell:
        rc_files = [Path.home() / ".zshrc"]
    elif "bash" in shell:
        rc_files = [Path.home() / ".bashrc"]
    else:
        rc_files = [Path.home() / ".profile"]

    for rc in rc_files:
        try:
            existing = rc.read_text(encoding="utf-8") if rc.exists() else ""
            if str(bin_dir) in existing:
                continue  # 已写入，跳过
            with rc.open("a", encoding="utf-8") as f:
                f.write(export_line)
            print(f"{GREEN}已将 {bin_dir} 追加到 {rc}{NC}")
        except OSError as error:
            print(f"{YELLOW}无法更新 {rc}: {error}{NC}")


def _broadcast_env_change() -> None:
    """通知已运行的进程环境变量发生变化（仅 Windows）。"""
    HWND_BROADCAST = 0xFFFF
    WM_SETTINGCHANGE = 0x1A
    try:
        ctypes.windll.user32.SendMessageW(
            HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment"
        )
    except Exception:
        pass


def _add_to_path_windows(bin_dir: Path) -> None:
    """通过注册表 HKCU\\Environment 持久化 PATH（幂等，无需管理员）。"""
    try:
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            "Environment",
            0,
            winreg.KEY_READ | winreg.KEY_WRITE,
        )
    except OSError as error:
        print(f"{YELLOW}无法打开注册表 Environment 键: {error}{NC}")
        return

    try:
        try:
            current, _ = winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            current = ""

        entries = [p for p in current.split(";") if p.strip()]
        target = str(bin_dir)
        if any(Path(p).resolve() == bin_dir.resolve() for p in entries if p):
            return  # 已存在

        new_path = ";".join(entries + [target])
        winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, new_path)
        print(f"{GREEN}已将 {target} 追加到用户 PATH{NC}")
    except OSError as error:
        print(f"{YELLOW}无法写入注册表 PATH: {error}{NC}")
    finally:
        winreg.CloseKey(key)

    _broadcast_env_change()


def _add_to_path(bin_dir: Path) -> None:
    """将 bin_dir 持久化到用户 PATH。"""
    if IS_WINDOWS:
        _add_to_path_windows(bin_dir)
    else:
        _add_to_path_unix(bin_dir)


def _write_alias(alias_path: Path, script_path: Path, interpreter: str) -> None:
    """写入 ipctool 包装脚本。"""
    if IS_WINDOWS:
        content = "@echo off\r\n" f'"{interpreter}" "{script_path}" %*\r\n'
        alias_path.write_text(content, encoding="ascii")
    else:
        content = "#!/bin/sh\n" f'exec "{interpreter}" "{script_path}" "$@"\n'
        alias_path.write_text(content, encoding="ascii")
        alias_path.chmod(0o755)


def env_alias_inject() -> None:
    """创建 ipctool 命令并将其所在目录加入用户 PATH。"""
    alias_dir = MANAGER_DIR / "local" / "bin"
    alias_path = alias_dir / ("ipctool.cmd" if IS_WINDOWS else "ipctool")
    script_path = FILE_PATH.resolve()
    interpreter = sys.executable or ("python" if IS_WINDOWS else "python3")

    # 1. 生成别名脚本
    try:
        alias_dir.mkdir(parents=True, exist_ok=True)
        _write_alias(alias_path, script_path, interpreter)
    except OSError as error:
        print(f"{YELLOW}无法创建 ipctool 命令: {error}{NC}")
        return

    # 2. 加入 PATH（持久化 + 当前进程）
    if not _is_in_path(alias_dir):
        _add_to_path(alias_dir)
        print(
            f"{YELLOW}请重开终端（或 source 相应配置文件）后 ipctool 才能全局可用。{NC}"
        )

    # 让当前进程及子进程立即生效
    if str(alias_dir) not in os.environ.get("PATH", "").split(os.pathsep):
        os.environ["PATH"] = str(alias_dir) + os.pathsep + os.environ.get("PATH", "")

    print(f"{GREEN}ipctool 已就绪：{alias_path}{NC}")


def scan_exec_dir():
    """扫描可执行文件目录，返回脚本列表。

    返回结构与 available_scripts() 一致：(name, ext, mode, desc)。
    可执行文件无扩展名，故 ext 为空串。
    """
    exec_dir_path: str = MANAGER_DIR / "build" / "app"
    scripts = []
    for item in os.listdir(exec_dir_path):
        item_path = os.path.join(exec_dir_path, item)

        # Only consider files
        if not os.path.isfile(item_path):
            continue

        # Avoid duplicates
        if any(script[0] == item for script in scripts):
            continue

        scripts.append((item, "", "exec", f"Executable tool {item}"))

    return scripts


# ---- Entry point ----


def main():
    scripts = available_scripts()
    env_alias_inject()
    scripts.extend(scan_tool_dir())
    scripts.extend(scan_exec_dir())
    if "--list" in sys.argv or "-l" in sys.argv:
        print_header()
        print(f"{BOLD}Available scripts:{NC}")
        print()
        print_scripts(scripts)
    elif "--help" in sys.argv or "-h" in sys.argv:
        print(f"Usage: python manage.py [--list|-l] [--help|-h]")
        print()
        print("  (no args)   Launch interactive TUI menu")
        print("  --list, -l  List available scripts and exit")
        print("  --help, -h  Show this help")
    else:
        try:
            main_loop(scripts)
        except KeyboardInterrupt:
            print()
            print(f"{GREEN}Goodbye!{NC}")
            sys.exit(0)


if __name__ == "__main__":
    main()
