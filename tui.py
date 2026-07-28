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

import os
import sys
import subprocess
import platform
import shutil
from pathlib import Path


# ---- Project paths ----
MANAGER_DIR = Path(__file__).resolve().parent
SCRIPTS_DIR = MANAGER_DIR / "scripts"
IS_WINDOWS = platform.system() == "Windows"

# ---- Script registry: name → {desc, mode} ----
# mode: "run" = execute directly, "src" = must be sourced (shell)/called (batch)
SCRIPT_REGISTRY = [
    {"name": "install",         "desc": "Full installation wizard (C++ / Python / msg-srv / dispatcher)", "mode": "run"},
    {"name": "setup",           "desc": "Activate dzIPC environment in current shell",                    "mode": "src"},
    {"name": "update_msg_srv",  "desc": "Sync & regenerate msg/srv files from external paths",            "mode": "run"},
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

BOLD   = _esc(1)
DIM    = _esc(2)
RED    = _esc(31)
GREEN  = _esc(32)
YELLOW = _esc(33)
BLUE   = _esc(34)
MAGENTA = _esc(35)
CYAN   = _esc(36)
NC     = _esc(0)

def with_tag(text, color):
    """Wrap text with a color tag, e.g. '[run]' in green."""
    return f"{color}{text}{NC}"

# ---- Box drawing helpers ----

H_LINE = "═"
V_LINE = "║"
TL     = "╔"
TR     = "╗"
BL     = "╚"
BR     = "╝"
T_JUNC = "╦"
B_JUNC = "╩"
V_RIGHT = "╠"
V_LEFT  = "╣"
CROSS   = "╬"
H_DIV   = "─" * 52

def draw_box_top():
    return f"{CYAN}{BOLD}{TL}{H_LINE * 52}{TR}{NC}"

def draw_box_bottom():
    return f"{CYAN}{BOLD}{BL}{H_LINE * 52}{BR}{NC}"

def draw_box_line(text):
    """Draw a single line inside the box."""
    # Estimate display width: ASCII=1, CJK=2
    w = sum(2 if ord(c) > 0x2E80 else 1 for c in text)
    pad = max(0, 52 - w)
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
    print(f"{DIM}  q) Quit    h) Help    l) List    r) Refresh" + f"    [1-{count}]{DIM} select{NC}")
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
    if mode == "run":
        return with_tag("[run]", GREEN)
    elif mode == "src":
        return with_tag(f"[src]", MAGENTA)
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
    print(f"  {GREEN}[run]{NC}  Script is executed directly from project root.")
    if IS_WINDOWS:
        print(f"  {MAGENTA}[src]{NC}  Batch file must be called (or sourced) from cmd.")
        print()
        print(f"  {BOLD}Tip:{NC} Run {GREEN}scripts\\setup.bat{NC} to activate the")
        print(f"       dzIPC environment in your current cmd session.")
    else:
        print(f"  {MAGENTA}[src]{NC}  Script must be sourced. You will be shown")
        print(f"         the exact command to copy-paste.")
        print()
        print(f"  {BOLD}Tip:{NC} Run {GREEN}source scripts/setup.sh{NC} to activate the")
        print(f"       dzIPC environment in your current shell.")
    print()
    print(f"  {BOLD}Tip:{NC} Use {GREEN}python manage.py --list{NC} for a quick overview.")
    print()
    input("Press Enter to return to menu...")

# ---- Run / execute a script ----

def run_script(name, ext, mode):
    script_path = SCRIPTS_DIR / f"{name}{ext}"

    if mode != "run":
        print()
        if IS_WINDOWS:
            print(f"{YELLOW}{BOLD}  {name}{ext} is a setup script.{NC}")
            print()
            print(f"  Please run this command directly in cmd.exe:")
            print()
            print(f"    {GREEN}{BOLD}scripts\\{name}{ext}{NC}")
        else:
            print(f"{YELLOW}{BOLD}  {name}{ext} must be SOURCED, not executed.{NC}")
            print()
            print(f"{BOLD}  Launching a new shell with dzIPC environment activated...{NC}")
            print(f"  {DIM}(Type 'exit' to return to your original shell.){NC}")
            print()
            # Replace the current Python process with a new bash that
            # sources the script, then drops into an interactive shell.
            # Environment vars set by the script persist because we use
            # exec, not spawn — the bash replaces this TUI process in-place.
            os.execvp("bash", [
                "bash",
                "-c",
                f"source '{script_path}' && exec bash -i",
            ])
        print()
        input("Press Enter to return to menu...")
        return

    # Mode "run": execute the script from project root
    print()
    if IS_WINDOWS:
        print(f"{GREEN}{BOLD}> Running:{NC} scripts\\{name}{ext}")
    else:
        print(f"{GREEN}{BOLD}> Running:{NC} bash scripts/{name}{ext}")
    print(f"{DIM}{H_DIV}{NC}")
    print()

    try:
        if IS_WINDOWS:
            result = subprocess.run(
                ["cmd", "/c", str(script_path)],
                cwd=str(MANAGER_DIR),
            )
        else:
            result = subprocess.run(
                ["bash", str(script_path)],
                cwd=str(MANAGER_DIR),
            )
    except KeyboardInterrupt:
        print()
        print(f"{YELLOW}Script interrupted by user.{NC}")
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

def main_loop():
    while True:
        scripts = available_scripts()
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

        if choice in ("q", "Q", "quit", "exit"):
            print()
            print(f"{GREEN}Goodbye!{NC}")
            sys.exit(0)
        elif choice in ("h", "H", "help"):
            print_help()
        elif choice in ("l", "L", "list"):
            print_header()
            print_scripts(scripts)
            input("Press Enter to return to menu...")
        elif choice in ("r", "R", "refresh"):
            continue
        elif choice.isdigit():
            idx = int(choice)
            if 1 <= idx <= len(scripts):
                run_script(*scripts[idx - 1][:3])
            else:
                print(f"{RED}Invalid selection: '{choice}'{NC}")
                import time
                time.sleep(1)
        else:
            print(f"{RED}Invalid selection: '{choice}'{NC}")
            import time
            time.sleep(1)

# ---- Entry point ----

def main():
    if "--list" in sys.argv or "-l" in sys.argv:
        print_header()
        scripts = available_scripts()
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
            main_loop()
        except KeyboardInterrupt:
            print()
            print(f"{GREEN}Goodbye!{NC}")
            sys.exit(0)

if __name__ == "__main__":
    main()
