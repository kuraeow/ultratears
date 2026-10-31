"""Run a heavy command on at most 8 logical CPUs at below-normal priority (the user works on this PC meanwhile).

usage: python tools/run_limited.py [--cpus N] -- <command> [args...]
The process starts suspended, gets the affinity mask and priority, then runs; its children inherit both.
MSYS2 CLANG64's bin directory is put first on PATH.
"""
import ctypes
import os
import subprocess
import sys


def main():
    args = sys.argv[1:]
    cpus = 8
    if args[:1] == ['--cpus']:
        cpus, args = int(args[1]), args[2:]
    if args[:1] == ['--']:
        args = args[1:]
    if sys.platform == "win32":
        env = dict(os.environ, PATH=r"C:\msys64\clang64\bin" + os.pathsep + os.environ.get("PATH", ""))
        p = subprocess.Popen(args, env=env, creationflags=0x4 | 0x4000)   # CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS
        if not ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t((1 << cpus) - 1)):
            p.kill()
            sys.exit("SetProcessAffinityMask failed")
        ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
        sys.exit(p.wait())
    else:
        def preexec():
            try:
                os.nice(10)
                if hasattr(os, "sched_setaffinity"):
                    os.sched_setaffinity(0, set(range(cpus)))
            except Exception:
                pass
        p = subprocess.Popen(args, preexec_fn=preexec)
        sys.exit(p.wait())


if __name__ == '__main__':
    main()
