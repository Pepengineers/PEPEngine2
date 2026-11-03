"""Convert the existing MGPU Luna soldier to glTF 2.0 using only the stdlib."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


def png_from_dds(source, destination):
    data = source.read_bytes()
    assert data[:4] == b"DDS " and len(data) >= 128, "Invalid DDS"
    height, width = struct.unpack_from("<II", data, 12)
    size, flags, fourcc, bits, red, green, blue, alpha = struct.unpack_from("<8I", data, 76)
    assert (size, fourcc, bits, red, green, blue) == (32, 0, 32, 0xFF0000, 0xFF00, 0xFF)
    assert flags & 0x40 and alpha in (0, 0xFF000000) and len(data) == 128 + width * height * 4
    pixels = bytearray(data[128:])
    pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
    if not flags & 1:
        pixels[3::4] = bytes([255]) * (width * height)
    rows = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
    destination.write_bytes(png + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def convert(source, output):
    model = source / "Models/soldier.m3d"
    tokens = iter(model.read_text(encoding="utf-8-sig").split())

    def label(expected):
        actual = next(tokens)
        assert actual == expected, (expected, actual)

    def numbers(count, cast=float):
        values = [cast(next(tokens)) for _ in range(count)]
        assert all(math.isfinite(v) for v in values)
        return values

    def field(name, count=1, cast=float):
        label(name)
        values = numbers(count, cast)
        return values[0] if count == 1 else values

    next(tokens)
    material_count = field("#Materials", cast=int)
    vertex_count = field("#Vertices", cast=int)
    triangle_count = field("#Triangles", cast=int)
    bone_count = field("#Bones", cast=int)
    clip_count = field("#AnimationClips", cast=int)
    assert (material_count, vertex_count, triangle_count, bone_count, clip_count) == (5, 13748, 22507, 58, 1)
    next(tokens)
    materials = []
    for _ in range(material_count):
        label("Name:")
        name = next(tokens)
        color = field("Diffuse:", 3)
        field("Fresnel0:", 3)
        roughness = field("Roughness:")
        clipped = field("AlphaClip:", cast=int)
        label("MaterialTypeName:")
        next(tokens)
        label("DiffuseMap:")
        diffuse = next(tokens)
        label("NormalMap:")
        normal = next(tokens)
        materials.append((name, color, roughness, clipped, diffuse, normal))
    next(tokens)
    subsets = []
    for index in range(material_count):
        assert field("SubsetID:", cast=int) == index
        subsets.append([field(name, cast=int) for name in ("VertexStart:", "VertexCount:", "FaceStart:", "FaceCount:")])
    next(tokens)
    vertices = []
    for _ in range(vertex_count):
        position = field("Position:", 3)
        tangent = field("Tangent:", 4)
        normal = field("Normal:", 3)
        uv = field("Tex-Coords:", 2)
        weights = field("BlendWeights:", 4)
        joints = field("BlendIndices:", 4, int)
        assert all(0 <= joint < bone_count for joint in joints)
        assert all(weight >= 0 for weight in weights) and abs(sum(weights) - 1) < 0.001
        weights = [weight / sum(weights) for weight in weights]
        vertices.append((position, normal, tangent, uv, joints, weights))
    next(tokens)
    indices = numbers(triangle_count * 3, int)
    assert min(indices) >= 0 and max(indices) < vertex_count
    next(tokens)
    inverse_binds = []
    for index in range(bone_count):
        label(f"BoneOffset{index}")
        matrix = numbers(16)
        assert all(abs(matrix[i]) < 1e-6 for i in (3, 7, 11)) and matrix[15] == 1
        inverse_binds.append(matrix)
    next(tokens)
    parents = [field(f"ParentIndexOfBone{index}:", cast=int) for index in range(bone_count)]
    assert all(-1 <= parent < index for index, parent in enumerate(parents))
    next(tokens)
    label("AnimationClip")
    clip_name = next(tokens)
    label("{")
    tracks = []
    for index in range(bone_count):
        label(f"Bone{index}")
        key_count = field("#Keyframes:", cast=int)
        label("{")
        keys = []
        for _ in range(key_count):
            time = field("Time:")
            position = field("Pos:", 3)
            scale = field("Scale:", 3)
            rotation = field("Quat:", 4)
            length = math.sqrt(sum(v * v for v in rotation))
            assert length > 0.99 and all(v > 0 for v in scale)
            keys.append((time, position, scale, [v / length for v in rotation]))
        assert keys[0][0] == 0 and all(a[0] < b[0] for a, b in zip(keys, keys[1:]))
        tracks.append(keys)
        label("}")
    label("}")
    assert next(tokens, None) is None and max(keys[-1][0] for keys in tracks) > 1
    assert any(keys[0][1:] != keys[len(keys) // 2][1:] for keys in tracks), "Animation is static"
    assert all(key[1] == tracks[0][0][1] for key in tracks[0]), "Source root motion would duplicate the App route"

    output.mkdir(parents=True, exist_ok=True)
    binary = bytearray()
    gltf = {"asset": {"version": "2.0", "generator": "PEPEngine2 MGPU soldier converter"},
            "scene": 0, "scenes": [{"nodes": [i for i, p in enumerate(parents) if p == -1] + [bone_count]}],
            "nodes": [], "meshes": [], "skins": [], "materials": [], "images": [], "textures": [],
            "animations": [], "bufferViews": [], "accessors": []}

    def accessor(values, width, kind="f", target=None, bounds=False):
        binary.extend(bytes((-len(binary)) % 4))
        offset = len(binary)
        for value in values:
            binary.extend(struct.pack("<" + kind * width, *(value if width > 1 else [value])))
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset}
        if target:
            view["target"] = target
        gltf["bufferViews"].append(view)
        record = {"bufferView": len(gltf["bufferViews"]) - 1, "count": len(values),
                  "componentType": {"f": 5126, "H": 5123}[kind],
                  "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}[width]}
        if bounds:
            columns = list(zip(*values)) if width > 1 else [values]
            record.update(min=[min(c) for c in columns], max=[max(c) for c in columns])
        gltf["accessors"].append(record)
        return len(gltf["accessors"]) - 1

    textures = {}
    source_hashes = {"Models/soldier.m3d": hashlib.sha256(model.read_bytes()).hexdigest()}
    for name, color, roughness, clipped, diffuse, normal in materials:
        slots = []
        for filename in (diffuse, normal):
            if filename.lower() not in textures:
                image_path = source / "Textures" / filename
                destination = Path(filename).stem + ".png"
                png_from_dds(image_path, output / destination)
                source_hashes["Textures/" + image_path.name] = hashlib.sha256(image_path.read_bytes()).hexdigest()
                textures[filename.lower()] = len(gltf["textures"])
                gltf["images"].append({"uri": destination})
                gltf["textures"].append({"source": len(gltf["images"]) - 1})
            slots.append(textures[filename.lower()])
        gltf["materials"].append({"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": color + [1], "metallicFactor": 0, "roughnessFactor": roughness,
            "baseColorTexture": {"index": slots[0]}}, "normalTexture": {"index": slots[1]},
            "alphaMode": "MASK" if clipped else "OPAQUE"})
    primitives = []
    for index, (start, count, face_start, face_count) in enumerate(subsets):
        attributes = {}
        for column, (name, width, kind) in enumerate((
                ("POSITION", 3, "f"), ("NORMAL", 3, "f"), ("TANGENT", 4, "f"),
                ("TEXCOORD_0", 2, "f"), ("JOINTS_0", 4, "H"), ("WEIGHTS_0", 4, "f"))):
            attributes[name] = accessor([v[column] for v in vertices[start:start + count]],
                                        width, kind, 34962, name == "POSITION")
        subset_indices = [value - start for value in indices[face_start * 3:(face_start + face_count) * 3]]
        assert min(subset_indices) >= 0 and max(subset_indices) < count
        primitives.append({"attributes": attributes, "indices": accessor(subset_indices, 1, "H", 34963), "material": index})
    gltf["meshes"].append({"name": "MGPUSoldier", "primitives": primitives})
    for index, keys in enumerate(tracks):
        node = {"name": f"SoldierBone{index}", "translation": keys[0][1], "scale": keys[0][2], "rotation": keys[0][3]}
        children = [i for i, parent in enumerate(parents) if parent == index]
        if children:
            node["children"] = children
        gltf["nodes"].append(node)
    gltf["nodes"].append({"name": "MGPUSoldier", "mesh": 0, "skin": 0})
    # M3D stores row-vector matrices; their row-major bytes equal glTF's transposed column-major bytes.
    gltf["skins"].append({"joints": list(range(bone_count)), "skeleton": parents.index(-1),
                          "inverseBindMatrices": accessor(inverse_binds, 16)})
    animation = {"name": clip_name, "samplers": [], "channels": []}
    for index, keys in enumerate(tracks):
        time = accessor([k[0] for k in keys], 1, bounds=True)
        for column, path, width in ((1, "translation", 3), (2, "scale", 3), (3, "rotation", 4)):
            animation["samplers"].append({"input": time, "output": accessor([k[column] for k in keys], width), "interpolation": "LINEAR"})
            animation["channels"].append({"sampler": len(animation["samplers"]) - 1, "target": {"node": index, "path": path}})
    gltf["animations"].append(animation)
    gltf["buffers"] = [{"uri": "Soldier.bin", "byteLength": len(binary)}]
    (output / "Soldier.bin").write_bytes(binary)
    (output / "Soldier.gltf").write_text(json.dumps(gltf, indent=2) + "\n", encoding="utf-8")
    (output / "source-sha256.json").write_text(json.dumps(source_hashes, indent=2) + "\n", encoding="utf-8")
    positions = [v[0] for v in vertices]
    print("Converted", vertex_count, "vertices,", triangle_count, "triangles,", bone_count, "bones,", clip_name)
    print("Source bounds:", [min(v[i] for v in positions) for i in range(3)], [max(v[i] for v in positions) for i in range(3)])
    print("All source counts, skin weights/indices, hierarchy, affine binds and animation checks passed.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="MGPU-Animations directory")
    parser.add_argument("output", type=Path, help="generated character asset directory")
    args = parser.parse_args()
    convert(args.source, args.output)
