"""Build the shader translator ultrs-shaders (MacNativeGames' totk_xlate service around the Ryujinx 1.3.2 shader
translator in third_party/maxwell_shader, both MIT) as one self-contained Windows program: build/shaders/ultrs-shaders.exe, which needs no .NET
installed to run (dev-local/build/shaders; tools/setup.py and tools/make_package.py take it from there). It turns the game's Switch GPU shaders (Maxwell machine code) into SPIR-V for Vulkan.

usage: python tools/build_xlate.py [--dotnet dotnet] [--out <folder>] [--cache <folder>]
Sources: third_party/shader_translator (the service; its patches/ are already applied to the copy in third_party/maxwell_shader,
kept as the record of what differs from Ryujinx 1.3.2). Needs the .NET 10 SDK and, the first time, network access for
the NuGet packages; everything it fetches stays inside the project (--cache, default dev-local: nuget, dotnet-home).
Next to the program goes source.txt, the stamp of the sources it was built from (tools/deps_install.py: a source
build rebuilds a translator whose stamp differs). The builder package carries the result, so players never run this.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--out', type=pathlib.Path, default=ROOT / 'dev-local/build/shaders')   # no game data, not in git
    ap.add_argument('--cache', type=pathlib.Path, default=ROOT / 'dev-local')
    a = ap.parse_args()
    out = a.out.resolve()
    env = dict(os.environ, NUGET_PACKAGES=str(a.cache / 'nuget'), DOTNET_CLI_HOME=str(a.cache / 'dotnet-home'),
               DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    proj = ROOT / 'third_party/shader_translator/ultrs-shaders.csproj'
    rid = 'linux-x64' if sys.platform.startswith('linux') else 'win-x64'
    rc = subprocess.run([a.dotnet, 'publish', str(proj), '-c', 'Release', '-r', rid, '--self-contained', 'true',
                         '-p:PublishSingleFile=true', '-p:InvariantGlobalization=true',
                         '-p:IncludeNativeLibrariesForSelfExtract=true',
                         f'-p:RyuSrc={ROOT / "third_party/maxwell_shader"}',
                         '-o', str(out), '-nologo', '-v:q', '-maxcpucount:4',
                         '--disable-build-servers'], env=env).returncode   # no MSBuild / compiler servers left running
    if rc:
        sys.exit(rc)
    exe = out / ('ultrs-shaders.exe' if rid.startswith('win') else 'ultrs-shaders')
    for f in out.iterdir():   # one program; the debug symbols are not needed
        if f.suffix == '.pdb':
            f.unlink()
    exe.chmod(0o755)
    sys.path.insert(0, str(ROOT / 'tools'))
    import deps_install
    (out / 'source.txt').write_text(deps_install.xlate_stamp(ROOT) + '\n')
    print(f'built {exe} ({exe.stat().st_size / 1e6:.0f} MB)')


if __name__ == '__main__':
    main()
