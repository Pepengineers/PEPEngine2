"""Run with python Apps/App/verify_scene.py; no GPU or extra packages required."""
from itertools import permutations
from math import isclose, prod
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCENE = ROOT / "Assets/Scenes/Amazon Lumberyard Bistro"


def read_materials(path):
    materials = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "newmtl":
            current = materials.setdefault(fields[1], {})
        else:
            current[fields[0]] = fields[1:]
    return materials


def composite(opaque, layers):
    # CPU reference for TransparentPass + independent RT blending + CompositionPass.
    accum = [0.0] * 4
    revealage = 1.0
    for color, alpha in layers:
        weight = max(0.01, alpha)
        for channel in range(3):
            accum[channel] += color[channel] * alpha * weight
        accum[3] += alpha * weight
        revealage *= 1.0 - alpha
    return tuple(opaque[i] * revealage + accum[i] / max(accum[3], 0.00001) * (1.0 - revealage)
                 for i in range(3))


class BistroChecks(unittest.TestCase):
    def test_material_texture_paths_exist(self):
        for part in ("Exterior", "Interior"):
            path = SCENE / part / (part.lower() + ".mtl")
            for name, material in read_materials(path).items():
                for slot, values in material.items():
                    if slot.startswith("map_") or slot in ("bump", "norm"):
                        texture = path.parent / " ".join(values).replace("\\", "/")
                        self.assertTrue(texture.is_file(), f"{part}/{name} {slot}: {texture}")

    def test_all_scene_parts_present(self):
        self.assertEqual({p.relative_to(SCENE).as_posix() for p in SCENE.rglob("*.obj")},
                         {"Exterior/exterior.obj", "Interior/interior.obj"})
        for part in ("Exterior/exterior", "Interior/interior"):
            self.assertGreater((SCENE / (part + ".obj")).stat().st_size, 1_000_000)
            self.assertTrue((SCENE / (part + ".mtl")).is_file())

    def test_bottle_glass_and_wine_enter_oit(self):
        for part, count in (("Exterior", 1), ("Interior", 5)):
            materials = read_materials(SCENE / part / (part.lower() + ".mtl"))
            glass = {name: values for name, values in materials.items()
                     if name.startswith("Paris_LiquorBottle_") and "Glass" in name}
            self.assertEqual(len(glass), count)
            self.assertIn("Paris_LiquorBottle_01_Glass_Wine", glass)
            for name, values in glass.items():
                opacity = float(values["d"][0])
                self.assertTrue(0.0 < opacity < 0.999, name)
                if "Tr" in values:
                    self.assertAlmostEqual(opacity, 1.0 - float(values["Tr"][0]), msg=name)
            self.assertEqual(float(materials["Paris_LiquorBottle_01_Caps"]["d"][0]), 1.0)

    def test_oit_empty_single_and_overlapping_layers(self):
        background = (0.2, 0.4, 0.8)
        self.assertEqual(composite(background, []), background)
        self.assertEqual(composite(background, [((1.0, 0.0, 0.0), 0.0)]), background)
        single = composite(background, [((1.0, 0.0, 0.0), 0.5)])
        for actual, expected in zip(single, (0.6, 0.2, 0.4)):
            self.assertAlmostEqual(actual, expected)
        layers = [((0.8, 0.1, 0.0), 0.65), ((0.0, 0.7, 0.1), 0.2), ((0.1, 0.2, 0.9), 0.25)]
        expected = composite(background, layers)
        for ordered in permutations(layers):
            self.assertTrue(all(isclose(a, b) for a, b in zip(composite(background, ordered), expected)))
        repeated = [((0.4, 0.5, 0.6), 0.25)] * 32
        coverage = 1.0 - prod(1.0 - alpha for _, alpha in repeated)
        for actual, bg, color in zip(composite(background, repeated), background, repeated[0][0]):
            self.assertAlmostEqual(actual, bg * (1.0 - coverage) + color * coverage)

    def test_all_glass_materials_have_partial_opacity(self):
        for part in ("Interior", "Exterior"):
            materials = read_materials(SCENE / part / (part.lower() + ".mtl"))
            glass = {name: values for name, values in materials.items() if "glass" in name.lower()}
            self.assertGreaterEqual(len(glass), 10)
            for name, values in glass.items():
                with self.subTest(part=part, material=name):
                    self.assertIn("d", values, "OIT glass needs explicit partial opacity")
                    opacity = float(values["d"][0])
                    self.assertTrue(0.0 < opacity < 0.999, name)
                    if "Tr" in values:
                        self.assertAlmostEqual(opacity, 1.0 - float(values["Tr"][0]), msg=name)


if __name__ == "__main__":
    unittest.main()
