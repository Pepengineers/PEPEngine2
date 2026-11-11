"""CPU-only shader checks. Set DXC and FXC to the Windows SDK compiler paths."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "Engine/Engine.RendererDX12/Shaders"
DXC = os.environ.get("DXC") or shutil.which("dxc")
FXC = os.environ.get("FXC") or shutil.which("fxc")


@unittest.skipUnless(DXC, "Set DXC to run shader compiler checks")
class LightingShaderTests(unittest.TestCase):
    def compile(self, source, entry, profile, *defines):
        output = ROOT / "Build/ShaderLightingScalarization"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            listing = Path(directory) / "shader.ll"
            args = [DXC, "-T", profile, "-E", entry, "-I", str(SHADERS),
                    "-Fo", str(Path(directory) / "shader.dxil"), "-Fc", str(listing)]
            for define in defines:
                args.extend(["-D", define])
            subprocess.run(args + [str(source)], check=True, capture_output=True, text=True)
            return listing.read_text()

    def test_baked_samples_surface_but_does_not_call_lighting(self):
        fixture = ROOT / "Apps/App/tests/shaders/BakedForwardContract.hlsl"
        for baked, expected_calls in [(1, 1), (0, 3)]:
            with self.subTest(baked=baked):
                listing = self.compile(fixture, "CS", "cs_6_0", f"TEST_BAKED={baked}")
                self.assertEqual(len(re.findall(r"call i32 @dx.op.atomicBinOp.i32", listing)),
                                 expected_calls)

    def test_vertex_outputs_only_export_used_fields(self):
        for name in ["OpaquePass", "TransparentPass"]:
            with self.subTest(shader=name):
                listing = self.compile(SHADERS / f"{name}.hlsl", "VS", "vs_6_0")
                signature = listing.split("; Output signature:", 1)[1].split("; shader hash:", 1)[0]
                self.assertNotIn("TANGENT", signature)
                if name == "TransparentPass":
                    self.assertNotRegex(signature, r"TEXCOORD\s+2\s")

    @unittest.skipUnless(FXC, "Set FXC to check SM5.1 probe culling")
    def test_probe_culling_sm51(self):
        output = ROOT / "Build/ShaderLightingScalarization"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            for entry in ["ClearCS", "CullCS"]:
                with self.subTest(entry=entry):
                    subprocess.run([FXC, "/nologo", "/T", "cs_5_1", "/E", entry,
                                    "/D", "PROBE_CULLING=1", "/Fo", str(Path(directory) / "shader.dxbc"),
                                    str(SHADERS / "ReflectionCapture.hlsl")],
                                   check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
