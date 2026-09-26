#!/usr/bin/env python3
"""Generator for the Aether Engine sample glTF content (content/samples/**).

The sample assets are tiny, hand-specified glTF 2.0 files used as the vertical-slice
test content and by the aether.assets unit tests. They are generated (rather than
hand-typed) because glTF embeds binary buffers as base64. Re-run this script after
editing it; it is deterministic, so unchanged inputs produce byte-identical files.

    python content/samples/generate_samples.py

Outputs (relative to this directory):
    cube/cube.gltf         24-vertex unit cube: POSITION/NORMAL/TEXCOORD_0 (no TANGENT, so
                           the importer's tangent generation is exercised), u16 indices,
                           one PBR metallic-roughness material with a base-color texture
                           embedded as a data URI and a metallic-roughness texture embedded
                           through a bufferView. Buffer embedded as a base64 data URI.
    cube/cube.glb          The same cube as binary glTF (images in bufferViews of the BIN chunk).
    skinned/skinned.gltf   A 3-segment vertical bar skinned to a 3-joint chain. The skin lists
                           its joints deliberately NOT in parent-before-child order
                           ([Tip, Root, Mid]) so the importer's topological sort + remap is
                           exercised. JOINTS_0 and WEIGHTS_0 are UNSIGNED_BYTE (weights
                           normalized). One looping animation "Wave" with a LINEAR rotation,
                           a STEP translation and a CUBICSPLINE scale channel.

Only the Python standard library is used (json, struct, zlib, base64, math).
"""

from __future__ import annotations

import base64
import json
import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
FLOAT = 5126
UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def png_rgba(width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> bytes:
    """Encode RGBA8 pixels (row-major, top row first) as a minimal PNG."""

    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: None
        for x in range(width):
            raw.extend(pixels[y * width + x])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def pad4(data: bytearray, fill: int = 0) -> None:
    while len(data) % 4:
        data.append(fill)


class BufferBuilder:
    """Accumulates one glTF buffer plus its bufferViews/accessors."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.buffer_views: list[dict] = []
        self.accessors: list[dict] = []

    def add_view(self, payload: bytes, target: int | None = None) -> int:
        pad4(self.data)
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        self.data.extend(payload)
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def add_accessor(self, payload: bytes, component_type: int, count: int, type_: str,
                     target: int | None = None, normalized: bool = False,
                     minmax: tuple[list, list] | None = None) -> int:
        view = self.add_view(payload, target)
        acc = {"bufferView": view, "componentType": component_type, "count": count, "type": type_}
        if normalized:
            acc["normalized"] = True
        if minmax is not None:
            acc["min"], acc["max"] = minmax
        self.accessors.append(acc)
        return len(self.accessors) - 1


def f32s(values) -> bytes:
    return struct.pack("<%df" % len(values), *values)


def vec_minmax(vectors: list[tuple[float, ...]]) -> tuple[list, list]:
    n = len(vectors[0])
    return ([min(v[i] for v in vectors) for i in range(n)],
            [max(v[i] for v in vectors) for i in range(n)])


def data_uri(mime: str, payload: bytes) -> str:
    return "data:%s;base64,%s" % (mime, base64.b64encode(payload).decode("ascii"))


def write_json(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("wrote", path.relative_to(HERE))


def write_glb(path: Path, doc: dict, bin_chunk: bytes) -> None:
    js = bytearray(json.dumps(doc, separators=(",", ":")).encode("utf-8"))
    pad4(js, 0x20)  # JSON chunk padded with spaces
    bn = bytearray(bin_chunk)
    pad4(bn, 0)
    total = 12 + 8 + len(js) + 8 + len(bn)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)          # "glTF", version 2
    out += struct.pack("<II", len(js), 0x4E4F534A) + js       # "JSON"
    out += struct.pack("<II", len(bn), 0x004E4942) + bn       # "BIN\0"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    print("wrote", path.relative_to(HERE))


ASSET = {"version": "2.0", "generator": "Aether generate_samples.py"}


# ---------------------------------------------------------------------------
# Cube
# ---------------------------------------------------------------------------
# Faces as (normal, right, up) with right x up == normal. Vertex order per face:
# bottom-left, bottom-right, top-right, top-left; UVs use glTF's top-left origin so the
# texture appears upright (non-mirrored) when each face is viewed from outside.
CUBE_FACES = [
    ((1, 0, 0), (0, 0, -1), (0, 1, 0)),
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0)),
]


def cube_geometry():
    positions, normals, uvs, indices = [], [], [], []
    corners = [(-1, -1, (0.0, 1.0)), (1, -1, (1.0, 1.0)), (1, 1, (1.0, 0.0)), (-1, 1, (0.0, 0.0))]
    for n, r, u in CUBE_FACES:
        base = len(positions)
        for sr, su, uv in corners:
            p = tuple(0.5 * n[i] + 0.5 * sr * r[i] + 0.5 * su * u[i] for i in range(3))
            positions.append(p)
            normals.append(tuple(float(c) for c in n))
            uvs.append(uv)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def checker_png() -> bytes:
    white, orange = (255, 255, 255, 255), (255, 128, 0, 255)
    px = [white if (x + y) % 2 == 0 else orange for y in range(4) for x in range(4)]
    return png_rgba(4, 4, px)


def metallic_roughness_png() -> bytes:
    # glTF packs roughness in G and metallic in B.
    px = [(255, 128, 0, 255), (255, 200, 0, 255), (255, 64, 255, 255), (255, 255, 255, 255)]
    return png_rgba(2, 2, px)


def build_cube(binary: bool) -> tuple[dict, bytes]:
    positions, normals, uvs, indices = cube_geometry()
    b = BufferBuilder()
    a_pos = b.add_accessor(f32s([c for p in positions for c in p]), FLOAT, 24, "VEC3",
                           ARRAY_BUFFER, minmax=vec_minmax(positions))
    a_nrm = b.add_accessor(f32s([c for p in normals for c in p]), FLOAT, 24, "VEC3", ARRAY_BUFFER)
    a_uv = b.add_accessor(f32s([c for p in uvs for c in p]), FLOAT, 24, "VEC2", ARRAY_BUFFER)
    a_idx = b.add_accessor(struct.pack("<%dH" % len(indices), *indices), UNSIGNED_SHORT,
                           len(indices), "SCALAR", ELEMENT_ARRAY_BUFFER)

    images = []
    checker = checker_png()
    if binary:
        images.append({"name": "Checker", "bufferView": b.add_view(checker), "mimeType": "image/png"})
    else:
        images.append({"name": "Checker", "uri": data_uri("image/png", checker)})
    images.append({"name": "MetalRough", "bufferView": b.add_view(metallic_roughness_png()),
                   "mimeType": "image/png"})
    pad4(b.data)

    buffer = {"byteLength": len(b.data)}
    if not binary:
        buffer["uri"] = data_uri("application/octet-stream", bytes(b.data))

    doc = {
        "asset": dict(ASSET),
        "scene": 0,
        "scenes": [{"name": "CubeScene", "nodes": [0]}],
        "nodes": [{"name": "Cube", "mesh": 0}],
        "meshes": [{
            "name": "Cube",
            "primitives": [{
                "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
                "indices": a_idx,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "CubeMaterial",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 0.8, 0.6, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.5,
                "metallicRoughnessTexture": {"index": 1},
            },
            "emissiveFactor": [0.0, 0.0, 0.0],
            "alphaMode": "OPAQUE",
            "doubleSided": False,
        }],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "textures": [{"sampler": 0, "source": 0}, {"sampler": 0, "source": 1}],
        "images": images,
        "buffers": [buffer],
        "bufferViews": b.buffer_views,
        "accessors": b.accessors,
    }
    return doc, bytes(b.data)


# ---------------------------------------------------------------------------
# Skinned bar
# ---------------------------------------------------------------------------
BAR_HALF = 0.1
BAR_SEGMENTS = 3  # along +Y, one unit each; joints at y = 0, 1, 2

# Skin joint order is intentionally NOT topological: [Tip, Root, Mid].
NODE_ARMATURE, NODE_ROOT, NODE_MID, NODE_TIP, NODE_MESH = 0, 1, 2, 3, 4
SKIN_JOINTS = [NODE_TIP, NODE_ROOT, NODE_MID]
SKIN_INDEX = {node: i for i, node in enumerate(SKIN_JOINTS)}


def bar_influences(y_level: int) -> tuple[list[int], list[int]]:
    """Joint (skin-index) and UNSIGNED_BYTE-normalized weights for a ring at y = y_level."""
    root, mid, tip = SKIN_INDEX[NODE_ROOT], SKIN_INDEX[NODE_MID], SKIN_INDEX[NODE_TIP]
    table = {
        0: ([root, 0, 0, 0], [255, 0, 0, 0]),
        1: ([root, mid, 0, 0], [128, 127, 0, 0]),
        2: ([mid, tip, 0, 0], [128, 127, 0, 0]),
        3: ([tip, 0, 0, 0], [255, 0, 0, 0]),
    }
    return table[y_level]


def bar_geometry():
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    h = BAR_HALF
    sides = [  # (normal, right) ; up is +Y
        ((0, 0, 1), (1, 0, 0)),
        ((1, 0, 0), (0, 0, -1)),
        ((0, 0, -1), (-1, 0, 0)),
        ((-1, 0, 0), (0, 0, 1)),
    ]
    for n, r in sides:
        base = len(positions)
        for level in range(BAR_SEGMENTS + 1):
            for side in (-1, 1):
                positions.append((n[0] * h + side * r[0] * h, float(level), n[2] * h + side * r[2] * h))
                normals.append(tuple(float(c) for c in n))
                uvs.append((0.0 if side < 0 else 1.0, 1.0 - level / BAR_SEGMENTS))
                j, w = bar_influences(level)
                joints.append(j)
                weights.append(w)
        for seg in range(BAR_SEGMENTS):
            bl, br = base + seg * 2, base + seg * 2 + 1
            tl, tr = bl + 2, br + 2
            indices += [bl, br, tr, bl, tr, tl]
    # Caps: bottom (y=0, normal -Y) and top (y=3, normal +Y).
    for y, ny, level in ((0.0, -1.0, 0), (float(BAR_SEGMENTS), 1.0, BAR_SEGMENTS)):
        base = len(positions)
        quad = [(-h, -h), (h, -h), (h, h), (-h, h)]  # (x, z)
        for x, z in quad:
            positions.append((x, y, z))
            normals.append((0.0, ny, 0.0))
            uvs.append((x / (2 * h) + 0.5, z / (2 * h) + 0.5))
            j, w = bar_influences(level)
            joints.append(j)
            weights.append(w)
        if ny > 0:  # CCW seen from +Y
            indices += [base, base + 3, base + 2, base, base + 2, base + 1]
        else:       # CCW seen from -Y
            indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, joints, weights, indices


def translation_matrix(tx: float, ty: float, tz: float) -> list[float]:
    # Column-major 4x4.
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1]


def quat_z(degrees: float) -> tuple[float, float, float, float]:
    half = math.radians(degrees) * 0.5
    return (0.0, 0.0, math.sin(half), math.cos(half))  # x, y, z, w


def build_skinned() -> dict:
    positions, normals, uvs, joints, weights, indices = bar_geometry()
    count = len(positions)
    b = BufferBuilder()
    a_pos = b.add_accessor(f32s([c for p in positions for c in p]), FLOAT, count, "VEC3",
                           ARRAY_BUFFER, minmax=vec_minmax(positions))
    a_nrm = b.add_accessor(f32s([c for p in normals for c in p]), FLOAT, count, "VEC3", ARRAY_BUFFER)
    a_uv = b.add_accessor(f32s([c for p in uvs for c in p]), FLOAT, count, "VEC2", ARRAY_BUFFER)
    a_jnt = b.add_accessor(bytes(c for j in joints for c in j), UNSIGNED_BYTE, count, "VEC4",
                           ARRAY_BUFFER)
    a_wgt = b.add_accessor(bytes(c for w in weights for c in w), UNSIGNED_BYTE, count, "VEC4",
                           ARRAY_BUFFER, normalized=True)
    a_idx = b.add_accessor(struct.pack("<%dH" % len(indices), *indices), UNSIGNED_SHORT,
                           len(indices), "SCALAR", ELEMENT_ARRAY_BUFFER)

    # Inverse bind matrices in SKIN joint order (joint world bind = translate(0, depth, 0)).
    joint_height = {NODE_ROOT: 0.0, NODE_MID: 1.0, NODE_TIP: 2.0}
    ibms = []
    for node in SKIN_JOINTS:
        ibms += translation_matrix(0.0, -joint_height[node], 0.0)
    a_ibm = b.add_accessor(f32s(ibms), FLOAT, len(SKIN_JOINTS), "MAT4")

    # Animation "Wave" (2 s, first key == last key so it loops seamlessly).
    rot_times = [0.0, 0.5, 1.0, 1.5, 2.0]
    rot_values = [quat_z(0), quat_z(30), quat_z(0), quat_z(-30), quat_z(0)]
    a_rot_t = b.add_accessor(f32s(rot_times), FLOAT, len(rot_times), "SCALAR",
                             minmax=([rot_times[0]], [rot_times[-1]]))
    a_rot_v = b.add_accessor(f32s([c for q in rot_values for c in q]), FLOAT, len(rot_values), "VEC4")

    step_times = [0.0, 1.0, 2.0]
    step_values = [(0.0, 0.0, 0.0), (0.0, 0.1, 0.0), (0.0, 0.0, 0.0)]
    a_step_t = b.add_accessor(f32s(step_times), FLOAT, 3, "SCALAR", minmax=([0.0], [2.0]))
    a_step_v = b.add_accessor(f32s([c for v in step_values for c in v]), FLOAT, 3, "VEC3")

    cubic_times = [0.0, 1.0, 2.0]
    zero, one, big = (0.0, 0.0, 0.0), (1.0, 1.0, 1.0), (1.2, 1.2, 1.2)
    cubic_values = [zero, one, zero, zero, big, zero, zero, one, zero]  # (in, value, out) per key
    a_cub_t = b.add_accessor(f32s(cubic_times), FLOAT, 3, "SCALAR", minmax=([0.0], [2.0]))
    a_cub_v = b.add_accessor(f32s([c for v in cubic_values for c in v]), FLOAT, 9, "VEC3")
    pad4(b.data)

    return {
        "asset": dict(ASSET),
        "scene": 0,
        "scenes": [{"name": "SkinnedScene", "nodes": [NODE_ARMATURE]}],
        "nodes": [
            {"name": "Armature", "children": [NODE_ROOT, NODE_MESH]},
            {"name": "Bone_Root", "children": [NODE_MID]},
            {"name": "Bone_Mid", "children": [NODE_TIP], "translation": [0.0, 1.0, 0.0]},
            {"name": "Bone_Tip", "translation": [0.0, 1.0, 0.0]},
            {"name": "SkinnedBar", "mesh": 0, "skin": 0},
        ],
        "skins": [{
            "name": "BarSkeleton",
            "inverseBindMatrices": a_ibm,
            "joints": SKIN_JOINTS,
            "skeleton": NODE_ROOT,
        }],
        "meshes": [{
            "name": "SkinnedBar",
            "primitives": [{
                "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv,
                               "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
                "indices": a_idx,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "BarMaterial",
            "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.6, 1.0, 1.0],
                                     "metallicFactor": 0.0, "roughnessFactor": 0.7},
        }],
        "animations": [{
            "name": "Wave",
            "samplers": [
                {"input": a_rot_t, "output": a_rot_v, "interpolation": "LINEAR"},
                {"input": a_step_t, "output": a_step_v, "interpolation": "STEP"},
                {"input": a_cub_t, "output": a_cub_v, "interpolation": "CUBICSPLINE"},
            ],
            "channels": [
                {"sampler": 0, "target": {"node": NODE_MID, "path": "rotation"}},
                {"sampler": 1, "target": {"node": NODE_ROOT, "path": "translation"}},
                {"sampler": 2, "target": {"node": NODE_TIP, "path": "scale"}},
            ],
        }],
        "buffers": [{"byteLength": len(b.data),
                     "uri": data_uri("application/octet-stream", bytes(b.data))}],
        "bufferViews": b.buffer_views,
        "accessors": b.accessors,
    }


def main() -> None:
    doc, _ = build_cube(binary=False)
    write_json(HERE / "cube" / "cube.gltf", doc)
    doc, bin_chunk = build_cube(binary=True)
    write_glb(HERE / "cube" / "cube.glb", doc, bin_chunk)
    write_json(HERE / "skinned" / "skinned.gltf", build_skinned())


if __name__ == "__main__":
    main()
