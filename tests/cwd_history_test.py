"""Verify the actual C++ integration commands in isolated interactive Bash histories."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--exe",required=True)
    parser.add_argument("--bash",default="C:/Program Files/Git/usr/bin/bash.exe")
    args=parser.parse_args()
    executable=Path(args.exe).resolve()
    bash=Path(args.bash).resolve()
    if not bash.is_file():
        parser.error("Bash executable not found; specify --bash")
    workspace=Path(__file__).resolve().parents[1]
    temporary_parent=workspace/"build_tmp"
    temporary_parent.mkdir(exist_ok=True)
    scenarios=[
        ("default",{},False),
        ("ignoredups",{"HISTCONTROL":"ignoredups"},False),
        ("ignorespace",{"HISTCONTROL":"ignorespace"},False),
        ("ignoreboth",{"HISTCONTROL":"ignoreboth"},False),
        ("duplicate-options",{"HISTCONTROL":"ignoredups:erasedups"},False),
        ("histignore",{"HISTIGNORE":"*__rshell_init_*"},False),
        ("timestamp",{"HISTTIMEFORMAT":"[%F %T] "},False),
        ("append-at-prompt",{"PROMPT_COMMAND":"history -a"},False),
        ("ignore-all",{"HISTIGNORE":"*"},False),
        ("history-disabled",{},True),
    ]
    with tempfile.TemporaryDirectory(prefix="cwd-history-",dir=temporary_parent) as temporary:
        directory=Path(temporary)
        command_file=directory/"commands.txt"
        subprocess.run([str(executable),"--history-commands",str(command_file)],check=True,timeout=15)
        commands=command_file.read_text(encoding="utf-8").splitlines()
        assert len(commands)==4
        assert all(command.startswith(" ") and "# __rshell_init_" in command for command in commands)
        user_commands=["printf '%s\\n' USER_BEFORE","printf '%s\\n' USER_BETWEEN","printf '%s\\n' USER_AFTER"]
        archived="printf '%s\\n' ARCHIVED_USER"
        old_integration="stty echo; printf '\\r\\033[2K'; printf '\\033]777;rshell-cwd;2:3\\007'"
        lookalike="printf '%s\\n' 'USER: stty echo; rshell-cwd;2:3'"
        for name,options,disabled in scenarios:
            scenario=directory/name
            scenario.mkdir()
            history=scenario/"history.txt"
            history.write_text(archived+"\n"+old_integration+"\n",encoding="utf-8")
            environment=os.environ.copy()
            for variable in ("HISTCONTROL","HISTIGNORE","HISTTIMEFORMAT","PROMPT_COMMAND","BASH_ENV","ENV","SHELLOPTS","BASHOPTS"):
                environment.pop(variable,None)
            environment.update(options)
            environment.update(HISTFILE=history.as_posix(),PS1="",PS2="")
            lines=(["set +o history"] if disabled else [])+[
                user_commands[0],commands[0],user_commands[1],commands[1],commands[2],commands[3],
                lookalike,user_commands[2],
                "printf '%s\\n' \"${HISTCONTROL-__unset__}\" \"${HISTIGNORE-__unset__}\" > settings.txt",
                "if [[ -o history ]]; then printf on; else printf off; fi > history-option.txt",
                "compgen -A function __rshell_hist_ > helper-functions.txt",
                "declare -p __rshell_line __rshell_number > helper-variables.txt 2>/dev/null",
                "builtin history -w history-snapshot.txt","exit",
            ]
            result=subprocess.run([str(bash),"--noprofile","--norc","-i"],input="\n".join(lines)+"\n",
                cwd=scenario,env=environment,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=20)
            if result.returncode:
                raise RuntimeError(f"{name}: Bash failed: {result.stderr}")
            saved=history.read_text(encoding="utf-8").splitlines()
            assert not any("__rshell_init_" in line for line in saved),(name,"new integration entry leaked")
            assert saved.count(archived)==1 and saved.count(old_integration)==1,(name,"pre-existing history changed")
            if not disabled and options.get("HISTIGNORE")!="*":
                assert all(saved.count(command)==1 for command in user_commands+[lookalike]),(name,"user command lost or duplicated",saved)
            settings=(scenario/"settings.txt").read_text(encoding="utf-8").splitlines()
            assert settings==[options.get("HISTCONTROL","__unset__"),options.get("HISTIGNORE","__unset__")],(name,"history settings changed")
            assert (scenario/"history-option.txt").read_text()==("off" if disabled else "on")
            assert (scenario/"helper-functions.txt").read_text()=="",(name,"temporary function leaked")
            assert (scenario/"helper-variables.txt").read_text()=="",(name,"temporary variables leaked")
            print(f"PASS: {name}: new initialization entries excluded; existing/user history and settings preserved")
    print(f"PASS: {len(scenarios)} real interactive Bash history configurations")


if __name__=="__main__":
    main()
