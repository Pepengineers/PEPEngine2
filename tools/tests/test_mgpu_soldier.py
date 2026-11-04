"""Check the converted soldier's front faces against its authored surface normals."""

import json
from pathlib import Path
import struct


def check(asset_directory):
    gltf = json.loads((asset_directory / "Soldier.gltf").read_text(encoding="utf-8"))
    data = (asset_directory / "Soldier.bin").read_bytes()

    def read(accessor_index):
        accessor = gltf["accessors"][accessor_index]
        view = gltf["bufferViews"][accessor["bufferView"]]
        width = {"SCALAR": 1, "VEC3": 3}[accessor["type"]]
        value_type = {5123: "H", 5126: "f"}[accessor["componentType"]]
        record = struct.Struct("<" + value_type * width)
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", record.size)
        return [record.unpack_from(data, offset + index * stride) for index in range(accessor["count"])]

    for primitive in gltf["meshes"][0]["primitives"]:
        positions = read(primitive["attributes"]["POSITION"])
        normals = read(primitive["attributes"]["NORMAL"])
        indices = [value[0] for value in read(primitive["indices"])]
        front = back = 0
        for start in range(0, len(indices), 3):
            triangle = indices[start:start + 3]
            a, b, c = [positions[index] for index in triangle]
            u = [b[axis] - a[axis] for axis in range(3)]
            v = [c[axis] - a[axis] for axis in range(3)]
            face = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                    u[0] * v[1] - u[1] * v[0])
            alignment = sum(face[axis] * sum(normals[index][axis] for index in triangle) for axis in range(3))
            front += alignment > 1e-8
            back += alignment < -1e-8
        name = gltf["materials"][primitive["material"]]["name"]
        # A few original faces disagree with smoothed normals; reversed whole surfaces must fail.
        assert front > 0.99 * (front + back), f"{name}: reversed front faces ({front} aligned, {back} reversed)"
        print(f"{name}: {front} front faces aligned with normals, {back} authored exceptions")


if __name__ == "__main__":
    check(Path(__file__).resolve().parents[2] / "Assets/Models/MGPUSoldier")
