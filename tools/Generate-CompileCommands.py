"""Export evaluated MSBuild C++ settings for clangd/Serena; does not build targets."""

import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def entries(project, data):
    props = data["Properties"]
    compiler = Path(props["VCToolsInstallDir"]) / "bin/Hostx64/x64/cl.exe"
    for item in data["Items"]["ClCompile"]:
        if item.get("ExcludedFromBuild", "").lower() == "true":
            continue
        source = Path(item["FullPath"])
        args = [str(compiler), "/nologo", "/c", "/EHsc"]
        runtime = {"multithreaded": "/MT", "multithreadeddebug": "/MTd",
                   "multithreadeddll": "/MD", "multithreadeddebugdll": "/MDd"}.get(item.get("RuntimeLibrary", "").lower())
        if runtime:
            args.append(runtime)
        standard = item.get("LanguageStandard", "")
        if standard:
            args.append("/std:" + standard.replace("stdcpp", "c++"))
        if item.get("CompileAs") == "CompileAsC":
            args.append("/TC")
        elif item.get("CompileAs") == "CompileAsCpp":
            args.append("/TP")
        for key, prefix in [("PreprocessorDefinitions", "/D"),
                            ("UndefinePreprocessorDefinitions", "/U"),
                            ("ForcedIncludeFiles", "/FI")]:
            args.extend(prefix + value.strip() for value in item.get(key, "").split(";") if value.strip())
        include_paths = item.get("AdditionalIncludeDirectories", "") + ";" + props["IncludePath"]
        for value in dict.fromkeys(p.strip() for p in include_paths.split(";") if p.strip()):
            path = Path(value)
            args.append("/I" + str(path if path.is_absolute() else (project.parent / path).resolve()))
        # Index source directly; MSVC PCH binaries and build/output flags are not clangd inputs.
        if "/utf-8" in item.get("AdditionalOptions", ""):
            args.append("/utf-8")
        args.append(str(source))
        if any("$(" in arg or "%(" in arg for arg in args):
            raise ValueError(f"Unresolved MSBuild expression in {project}: {source}")
        if not source.is_file():
            raise FileNotFoundError(source)
        yield {"directory": str(project.parent), "file": str(source), "arguments": args}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--msbuild", type=Path)
    args = parser.parse_args()
    msbuild = args.msbuild
    if msbuild is None:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        found = subprocess.check_output([str(vswhere), "-latest", "-requires", "Microsoft.Component.MSBuild",
                                         "-find", "MSBuild/**/Bin/MSBuild.exe"], text=True).splitlines()
        if not found:
            raise RuntimeError("MSBuild not found; pass --msbuild with its absolute path")
        msbuild = Path(found[0])
    projects = sorted([p for folder in ["Engine", "Apps"] for p in (ROOT / folder).rglob("*.vcxproj")],
                      key=lambda p: ("Tests" in p.stem, str(p)))
    database = {}
    for project in projects:
        command = [str(msbuild), str(project), "/nologo", "/v:quiet", "/p:Platform=x64",
                   f"/p:Configuration={args.configuration}", "-getItem:ClCompile",
                   "-getProperty:IncludePath,VCToolsInstallDir"]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, encoding="utf-8-sig", check=True)
        for entry in entries(project, json.loads(result.stdout)):
            # Shared sources use their first production configuration before test-project variants.
            database.setdefault(os.path.normcase(entry["file"]), entry)
    if not database:
        raise RuntimeError("MSBuild returned no translation units")
    output = ROOT / "compile_commands.json"
    temporary = output.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(list(database.values()), indent=2) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(json.dumps({"projects": len(projects), "translation_units": len(database),
                      "configuration": args.configuration, "output": str(output)}))


if __name__ == "__main__":
    main()
