"""Check that the clangd export preserves parsing inputs and rejects stale sources."""

import copy
from pathlib import Path
import runpy
import tempfile
import unittest


entries = runpy.run_path(str(Path(__file__).resolve().parents[1] / "Generate-CompileCommands.py"))["entries"]


class CompileCommandsTest(unittest.TestCase):
    def test_evaluated_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source with spaces.cpp"
            source.touch()
            project = root / "example.vcxproj"
            data = {"Properties": {"VCToolsInstallDir": str(root), "IncludePath": "SDK include;"},
                    "Items": {"ClCompile": [{"FullPath": str(source), "LanguageStandard": "stdcpp17",
                                            "RuntimeLibrary": "MultiThreadedDebugDll",
                                            "PreprocessorDefinitions": ' FEATURE=1 ;LABEL="two words"; ',
                                            "AdditionalIncludeDirectories": "project include;",
                                            "ForcedIncludeFiles": "forced.h", "AdditionalOptions": "/utf-8"}]}}
            result = list(entries(project, data))
            self.assertEqual(len(result), 1)
            args = result[0]["arguments"]
            for flag in ["/std:c++17", "/MDd", "/DFEATURE=1", '/DLABEL="two words"', "/FIforced.h",
                         "/I" + str(root / "SDK include"), "/I" + str(root / "project include"), "/utf-8"]:
                self.assertIn(flag, args)
            self.assertEqual(args[-1], str(source))

            excluded = copy.deepcopy(data)
            excluded["Items"]["ClCompile"][0].update(ExcludedFromBuild="true", FullPath="missing.cpp")
            self.assertEqual(list(entries(project, excluded)), [])
            unresolved = copy.deepcopy(data)
            unresolved["Items"]["ClCompile"][0]["PreprocessorDefinitions"] = "$(MissingProperty)"
            with self.assertRaises(ValueError):
                list(entries(project, unresolved))
            source.unlink()
            with self.assertRaises(FileNotFoundError):
                list(entries(project, data))


if __name__ == "__main__":
    unittest.main()
