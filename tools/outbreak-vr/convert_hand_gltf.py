#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
# SPDX-License-Identifier: GPL-3.0
"""Bake the Godot XR Tools low-poly hand (CC0 1.0) into fixed poses for the VR hand renderer.

    python -I tools/outbreak-vr/convert_hand_gltf.py --left Hand_low_L.gltf --right Hand_low_R.gltf \
        --out pcsx2/VR/HandMeshData.inc [--preview poses.png]

The inputs are Hand_low_L.gltf and Hand_low_R.gltf from GodotVR/godot-xr-tools, addons/godot-xr-tools/hands/model/
(CC0 1.0, Miodrag Sejic aka DigitalN8m4r3; not committed here, download them and pass their paths). Each is one skinned
mesh (603 vertices, 938 triangles) on 26 joints named like OpenXR's XR_EXT_hand_tracking joints, with the buffer embedded
as a data: URI (a .bin next to the glTF works too). Pure Python 3 and numpy; no Blender. The files are untrusted data:
only JSON, accessors and a buffer are read from them, and an external buffer must sit next to the glTF.

The glTF carries no animations. The hand's own poses (Grip, Pistol, Grip Shaft, Hold, Point...) ship only as Godot .res
files and in the .blend files, which this does not use, so the poses are authored here the way the procedural hand in
pcsx2/VR/HandModel.cpp poses its own: per bone a target direction in HandModel's handle frame (a finger's bends about
the handle axis, the thumb's directions), solved on the glTF's skeleton with its own bone lengths and then skinned
(linear blend, the glTF's weights). The numbers are in POSES below, next to HandModel's, to tune.

The output is in HandModel's pointing frame, the one HandState's pos and quat place; see the header the .inc gets.
"""

import argparse
import base64
import json
import math
import os
import sys

import numpy as np

# ---- The output frame ------------------------------------------------------------------------------------------------
#
# HandModel builds the right hand in a "handle frame": the controller's handle (or a gun's grip) runs along +Y through the
# origin, -Z is forward (the way open fingers point), the palm faces -X (it lies on the +X side of the handle, the back of
# the hand toward +X) and the thumb is on the +Y side; the fingers curl about the handle axis, from -Z toward -X. BuildWorld
# then tilts that frame about X so the top of the handle leans forward by kHandleTiltDeg, mirrors X for the left hand, and
# puts the result at HandState::pos turned by HandState::quat (the pointing frame: -Z forward along the barrel, +Y up, +X
# right). This script bakes straight into the tilted frame, and takes the left hand from the left glTF instead of mirroring.
HANDLE_TILT_DEG = 15.0  # HandModel.cpp kHandleTiltDeg
# HandModel's palm also runs back from the knuckles this far below square to the handle (kPalmDropDeg), so after the tilt
# the wrist comes out level instead of cocked up. The Godot hand is straight, so its wrist joint is moved down by the same
# angle (the joints below it keep their places) and the skin skews the palm's back half with it.
PALM_DROP_DEG = 15.0

# Where the Godot hand sits in the handle frame (right hand, centimetres): the mean of its four knuckles (the MCP joints,
# the *_Proximal joints in OpenXR's naming) lands where the mean of HandModel's four kFingers[].knuckle is. The palm's
# inner face then lies on the handle's +X face (x = 1.3 cm) across the knuckles, and the back of the hand on the 3 cm
# slab HandModel's own palm fills (x 1.3 to 4.3).
KNUCKLE_MEAN_CM = np.array([2.8, 0.05, -2.2])

FINGERS = ("Index", "Middle", "Ring", "Little")
POSE_NAMES = ("OPEN", "GRIP", "PISTOL", "POINT")  # the order of HandMesh::Pose

# Per pose and finger the bends in degrees at (MCP, PIP, DIP), added up along the finger about the handle axis from -Z
# toward the palm: the directions of its three bones are -Z turned by the first, by the first two, and by all three.
# None leaves the finger as the glTF rests it. HandModel's own closed finger is {78, 95, 62} and its Gun index {30..40,
# 95..110, 35..50}; the Godot fingers are thicker (about 2 cm) and wrap the same 2.6 x 3.2 cm handle, so they close a
# little less at the tip: their joints then lie about a finger's half-thickness off the handle's faces, not in it.
CLOSED = (85.0, 75.0, 40.0)
CLOSED_LITTLE = (80.0, 80.0, 40.0)  # its knuckle is further back, so it starts nearer the handle
FIST = {"Index": CLOSED, "Middle": CLOSED, "Ring": CLOSED, "Little": CLOSED_LITTLE}

# Thumb bone directions (metacarpal, proximal, distal) in the handle frame, right hand (X is mirrored for the left). The
# Godot thumb's base sits further out than HandModel's and its bones are longer (4.5, 4.2, 3.1 cm against 3.6, 3.0, 2.5),
# so HandModel's kThumbFist and kThumbGun directions would put it 3 cm too far to the left; these are those, turned
# until the thumb lies where theirs does: over the fingers for a fist, along the left of the frame for the pistol.
THUMB_FIST = ((-0.55, -0.10, -0.83), (-0.20, -0.20, -0.96), (0.75, -0.10, -0.65))
THUMB_FRAME = ((-0.36, 0.18, -0.93), (-0.23, 0.05, -0.97), (-0.07, -0.07, -1.00))

POSES = {
    # OPEN: the glTF's own relaxed pose.
    "OPEN": {"fingers": {f: None for f in FINGERS}, "thumb": None},
    # GRIP: a fist round a handle or shaft, the thumb over the fingers.
    "GRIP": {"fingers": FIST, "thumb": THUMB_FIST},
    # PISTOL: the index inside the trigger guard in front of the trigger (as HandModel's), the others round the grip,
    # the thumb along the frame.
    "PISTOL": {"fingers": {**FIST, "Index": (30.0, 95.0, 35.0)}, "thumb": THUMB_FRAME},
    # POINT: the index straight out along the knuckle line, the other three closed, the thumb over them.
    "POINT": {"fingers": {**FIST, "Index": (0.0, 0.0, 0.0)}, "thumb": THUMB_FIST},
}


# ---- glTF reading ----------------------------------------------------------------------------------------------------
class Gltf:
    """The one skinned mesh of a Hand_low_*.gltf: its bind-pose vertices and weights, indices and skeleton."""

    COMPONENT = {5120: "i1", 5121: "u1", 5122: "<i2", 5123: "<u2", 5125: "<u4", 5126: "<f4"}
    WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

    def __init__(self, path):
        with open(path, "r", encoding="utf-8") as f:
            self.doc = json.load(f)
        base = os.path.dirname(os.path.abspath(path))
        self.buffers = [self._load_buffer(b, base) for b in self.doc["buffers"]]
        meshes = self.doc["meshes"]
        if len(meshes) != 1 or len(meshes[0]["primitives"]) != 1:
            raise SystemExit(f"{path}: expected one mesh with one primitive")
        prim = meshes[0]["primitives"][0]
        if prim.get("mode", 4) != 4:
            raise SystemExit(f"{path}: the primitive is not a triangle list")
        attrs = prim["attributes"]
        self.position = self._accessor(attrs["POSITION"]).astype(np.float64)
        self.normal = self._accessor(attrs["NORMAL"]).astype(np.float64)
        self.joints = self._accessor(attrs["JOINTS_0"]).astype(np.int64)
        self.weights = self._accessor(attrs["WEIGHTS_0"]).astype(np.float64)
        self.weights /= self.weights.sum(axis=1, keepdims=True)
        self.index = self._accessor(prim["indices"]).astype(np.int64).ravel()
        if len(self.index) % 3 or self.index.max() >= len(self.position) or len(self.position) > 65535:
            raise SystemExit(f"{path}: bad index list")
        self.animations = [a.get("name", "?") for a in self.doc.get("animations", [])]

        skin = self.doc["skins"][0]
        nodes = self.doc["nodes"]
        self.joint_nodes = skin["joints"]
        # inverseBindMatrices are column-major 4x4: transpose each into row-vector-free (M @ v) form
        self.ibm = self._accessor(skin["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
        parent = {}
        for i, n in enumerate(nodes):
            for c in n.get("children", []):
                parent[c] = i
        joint_index = {n: k for k, n in enumerate(self.joint_nodes)}
        # "Index_Proximal_R" -> "Index_Proximal", "Wrist_L" -> "Wrist"
        self.name = [nodes[n]["name"][:-2] if nodes[n]["name"][-2:] in ("_R", "_L") else nodes[n]["name"]
                     for n in self.joint_nodes]
        self.joint_of = {nm: k for k, nm in enumerate(self.name)}
        self.local_t = [np.array(nodes[n].get("translation", [0, 0, 0]), dtype=np.float64) for n in self.joint_nodes]
        self.local_q = [np.array(nodes[n].get("rotation", [0, 0, 0, 1]), dtype=np.float64) for n in self.joint_nodes]
        self.local_s = [np.array(nodes[n].get("scale", [1, 1, 1]), dtype=np.float64) for n in self.joint_nodes]
        # The parent joint of each joint, -1 for the skeleton's root; whatever sits above it (the Armature node) is folded
        # into root_world.
        self.parent_joint = [joint_index.get(parent.get(n), -1) for n in self.joint_nodes]
        roots = [k for k, p in enumerate(self.parent_joint) if p < 0]
        if len(roots) != 1:
            raise SystemExit(f"{path}: expected one root joint, found {len(roots)}")
        self.root_world = np.eye(4)
        chain = []
        p = parent.get(self.joint_nodes[roots[0]])
        while p is not None:
            chain.append(p)
            p = parent.get(p)
        for p in reversed(chain):
            node = nodes[p]
            m = np.eye(4)
            m[:3, :3] = quat_to_mat(node.get("rotation", [0, 0, 0, 1])) * np.array(node.get("scale", [1, 1, 1]))[None, :]
            m[:3, 3] = node.get("translation", [0, 0, 0])
            self.root_world = self.root_world @ m

    @staticmethod
    def _load_buffer(b, base):
        uri = b.get("uri", "")
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1])
        if not uri or os.path.basename(uri) != uri or uri in (".", ".."):
            raise SystemExit(f"unsupported buffer uri {uri!r}: expected a data: URI or a file next to the glTF")
        with open(os.path.join(base, uri), "rb") as f:
            data = f.read()
        if len(data) < b["byteLength"]:
            raise SystemExit(f"{uri}: shorter than the glTF says")
        return data

    def _accessor(self, i):
        a = self.doc["accessors"][i]
        bv = self.doc["bufferViews"][a["bufferView"]]
        dtype = np.dtype(self.COMPONENT[a["componentType"]])
        width = self.WIDTH[a["type"]]
        if bv.get("byteStride") not in (None, dtype.itemsize * width):
            raise SystemExit("interleaved buffer views are not supported")
        offset = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        data = np.frombuffer(self.buffers[bv["buffer"]], dtype=dtype, count=a["count"] * width, offset=offset)
        return data.reshape(a["count"], width)

    # -- posing --
    def world_matrices(self, local_q):
        """Joint world matrices (model space) for the given local rotations (quaternions xyzw); translations as glTF."""
        world = [None] * len(self.joint_nodes)

        def build(k):
            if world[k] is None:
                m = np.eye(4)
                m[:3, :3] = quat_to_mat(local_q[k]) * self.local_s[k][None, :]
                m[:3, 3] = self.local_t[k]
                p = self.parent_joint[k]
                world[k] = (build(p) if p >= 0 else self.root_world) @ m
            return world[k]

        for k in range(len(world)):
            build(k)
        return world

    def skin(self, local_q):
        """Linear blend skinning: the posed positions and unit normals (model space) for the given local rotations."""
        world = self.world_matrices(local_q)
        skin_m = np.stack([world[k] @ self.ibm[k] for k in range(len(world))])
        pos = np.zeros_like(self.position)
        nrm = np.zeros_like(self.normal)
        for slot in range(4):
            w = self.weights[:, slot]
            m = skin_m[self.joints[:, slot]]
            pos += w[:, None] * (np.einsum("nij,nj->ni", m[:, :3, :3], self.position) + m[:, :3, 3])
            nrm += w[:, None] * np.einsum("nij,nj->ni", m[:, :3, :3], self.normal)
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        return pos, nrm, world


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def mat_to_quat(m):
    t = m[0, 0] + m[1, 1] + m[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def rot_between(a, b):
    """The shortest rotation matrix taking the direction a to the direction b."""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    c = float(np.dot(a, b))
    if c < -0.999999:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2.0 * np.outer(axis, axis) - np.eye(3)
    v = np.cross(a, b)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1.0 + c)


def rot_x(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


# ---- Posing ----------------------------------------------------------------------------------------------------------
def aim_chain(g, local_q, chain, targets):
    """Local rotations (one quaternion per joint) that point each bone of the chain along its target direction.

    chain: joint names, each the parent of the next; targets: one direction in model space per joint but the last, for
    the bone from that joint to the next. Each joint's world rotation is turned by the shortest arc from where its bone
    points (with its parents already posed) to the target, so the roll the glTF gives it is kept.
    """
    local_q = [q.copy() for q in local_q]
    for step, target in enumerate(targets):
        k = g.joint_of[chain[step]]
        child = g.joint_of[chain[step + 1]]
        world = g.world_matrices(local_q)
        bone = world[child][:3, 3] - world[k][:3, 3]
        turn = rot_between(bone, target)
        parent = g.parent_joint[k]
        parent_rot = world[parent][:3, :3] if parent >= 0 else g.root_world[:3, :3]
        parent_rot = parent_rot / np.linalg.norm(parent_rot, axis=0, keepdims=True)  # without the scale
        joint_rot = world[k][:3, :3] / np.linalg.norm(world[k][:3, :3], axis=0, keepdims=True)
        local_q[k] = mat_to_quat(parent_rot.T @ turn @ joint_rot)
    return local_q


class Hand:
    """One side: the glTF, where its native frame sits in the output frame, and its poses."""

    def __init__(self, path, left):
        self.path = path
        self.left = left
        self.side = -1.0 if left else 1.0  # the left handle frame is the right one mirrored in X
        self.g = Gltf(path)
        g = self.g
        rest_world = g.world_matrices(g.local_q)
        knuckles = np.array([rest_world[g.joint_of[f + "_Proximal"]][:3, 3] for f in FINGERS])
        target = KNUCKLE_MEAN_CM / 100.0 * np.array([self.side, 1.0, 1.0])
        self.shift = target - knuckles.mean(axis=0)  # native frame -> handle frame (a translation)
        self.tilt = rot_x(-HANDLE_TILT_DEG)  # handle frame -> pointing frame: HandModel's ta = -kHandleTiltDeg

        # The palm drop: the wrist joint goes where HandModel's palm ends, back along (0, -sin a, cos a) from the knuckles
        # (a = PALM_DROP_DEG), and every joint under it keeps its place, so only what the wrist's skin weights hold moves.
        wrist = g.joint_of["Wrist"]
        back = rest_world[wrist][2, 3] - knuckles.mean(axis=0)[2]
        a = math.radians(PALM_DROP_DEG)
        move = np.array([0.0, -back * math.sin(a), back * (math.cos(a) - 1.0)])
        wrist_rot = rest_world[wrist][:3, :3]
        g.local_t[wrist] = g.local_t[wrist] + np.linalg.inv(g.root_world[:3, :3]) @ move
        for child in [k for k, p in enumerate(g.parent_joint) if p == wrist]:
            g.local_t[child] = g.local_t[child] - np.linalg.inv(wrist_rot) @ move

    def solve(self, pose):
        """The local joint rotations for a pose (native directions equal handle-frame ones: the shift is a translation)."""
        g = self.g
        spec = POSES[pose]
        local_q = [q.copy() for q in g.local_q]
        for f in FINGERS:
            bends = spec["fingers"][f]
            if bends is None:
                continue
            dirs = []
            total = 0.0
            for bend in bends:
                total += bend
                # curl about the handle axis from -Z toward the palm: toward -X for the right hand, +X for the left
                dirs.append(rot_y(total * self.side) @ np.array([0.0, 0.0, -1.0]))
            local_q = aim_chain(g, local_q, [f + "_Proximal", f + "_Intermediate", f + "_Distal", f + "_Tip"], dirs)
        if spec["thumb"] is not None:
            dirs = [unit([d[0] * self.side, d[1], d[2]]) for d in spec["thumb"]]
            local_q = aim_chain(g, local_q, ["Thumb_Metacarpal", "Thumb_Proximal", "Thumb_Distal", "Thumb_Tip"], dirs)
        return local_q

    def bake(self, pose):
        """Positions (metres) and unit normals in the pointing frame, and the anchors, for a pose."""
        pos, nrm, world = self.g.skin(self.solve(pose))
        pos = (pos + self.shift) @ self.tilt.T
        nrm = nrm @ self.tilt.T
        anchors = {}
        for key, joint in (("wrist", "Wrist"), ("index_tip", "Index_Tip")):
            anchors[key] = (world[self.g.joint_of[joint]][:3, 3] + self.shift) @ self.tilt.T
        return pos, nrm, anchors


# ---- Output ----------------------------------------------------------------------------------------------------------
LICENCE_HEADER = """\
// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0
//
// HandMeshData.inc: the low-poly hand of Godot XR Tools, baked into four poses per side. GENERATED by
// tools/outbreak-vr/convert_hand_gltf.py from Hand_low_L.gltf and Hand_low_R.gltf; do not edit it, run the script again.
//
// The hand meshes this is derived from are CC0 1.0 Universal (a public domain dedication), from GodotVR/godot-xr-tools,
// addons/godot-xr-tools/hands/. The licence file there (License.md) begins:
//
//   To the extent possible under law, DigitalN8m4r3 aka Miodrag Sejic
//   has waived all copyright and related or neighboring rights to Hand Models for the Godot XR Tools.
//   This work is published from: Austria.
//   Date: 7th November 2022
//
// and goes on with the CC0 1.0 Universal legal code (https://creativecommons.org/publicdomain/zero/1.0/legalcode). The
// hands' base mesh is from MakeHuman's CC0 assets (hands/About.md). CC0 asks for no credit; it is given with thanks. The
// poses, the frame and the layout below are this project's work, under the licence of the rest of this code.
//
// FRAME. Metres, in the pointing frame that HandModel::HandState places a hand in (its pos and quat): +X right, +Y up, -Z
// forward along the barrel, the origin at the controller's grip position, inside the handle, where HandModel's own hand
// has it. The handle, or a gun's grip, runs through the origin along +Y turned 15 degrees (HandModel's kHandleTiltDeg: its
// top leans forward, toward -Z), and the palm drop is HandModel's too (kPalmDropDeg: the wrist comes out about level
// behind the knuckles).
//
//   Right hand: the fingers point forward (-Z) and, open, about 20 degrees below level; the thumb is on the +Y side; the
//   palm faces -X (it lies against the +X side of the handle, the back of the hand toward +X). The mean of the four
//   knuckles is at (2.8, -0.5, -2.1) cm, as HandModel's: 2.8 cm to the right of the handle's axis and 2.1 cm ahead of it.
//   The wrist joint is at about (1.6, 0.6, 7.7) cm, and the middle of the palm (halfway from the wrist to the middle
//   knuckle, where an OpenXR hand-tracking Palm joint would be) at about (2.5, 0.4, 2.4) cm from the origin.
//   Left hand: the same with X mirrored (the palm faces +X), from the left glTF rather than mirrored here.
//
// The mesh needs no rotation against the pointing frame: draw it at pos, turned by quat. The OPEN hand is 21 cm long (z
// -13.0 to +7.9 cm).
//
// POSES (HandMesh::Pose order). OPEN: the glTF's own relaxed rest pose. GRIP: a fist round a handle or shaft, the thumb
// over the fingers. PISTOL: the index inside the trigger guard in front of the trigger, the other fingers round the grip,
// the thumb along the left of the frame (right hand). POINT: the index straight out, the other fingers closed, the thumb
// over them. The glTF carries no animations (Godot keeps its Grip, Pistol, Hold... poses in .res and .blend files, which
// are not used), so the script solves these from bend angles and bone directions on the glTF's skeleton, with HandModel's
// numbers as the start, and skins the mesh with the glTF's weights (linear blend).
//
// LAYOUT. Per side one index list shared by the poses (u16, three per triangle, counter-clockwise seen from outside), per
// pose the vertex positions and the unit normals (three floats each, in vertex order), and per pose the wrist joint and
// the index fingertip (x, y, z, x, y, z), for a cuff and a pointing ray.
"""


def fmt_floats(values, decimals, per_line=12, indent="\t\t"):
    out = []
    flat = np.round(np.asarray(values, dtype=np.float64).ravel(), decimals)
    text = [f"{(0.0 if v == 0 else v):.{decimals}f}f" for v in flat]
    for i in range(0, len(text), per_line):
        out.append(indent + ", ".join(text[i:i + per_line]) + ",")
    return "\n".join(out)


def fmt_ints(values, per_line=24, indent="\t\t"):
    text = [str(int(v)) for v in values]
    return "\n".join(indent + ", ".join(text[i:i + per_line]) + "," for i in range(0, len(text), per_line))


def write_inc(path, hands, baked):
    """hands: {"Left": Hand, "Right": Hand}; baked: {side: {pose: (pos, nrm, anchors)}}."""
    nv = len(hands["Right"].g.position)
    nt = len(hands["Right"].g.index) // 3
    for h in hands.values():
        if len(h.g.position) != nv or len(h.g.index) // 3 != nt:
            raise SystemExit("the two hands differ in size")
    lines = [LICENCE_HEADER.rstrip("\n"), "", "// The mesh: vertices and triangles of each side (the same count on both), and the poses.",
             f"constexpr u32 kHandVertexCount = {nv};", f"constexpr u32 kHandTriangleCount = {nt};",
             f"constexpr u32 kHandPoseCount = {len(POSE_NAMES)};", ""]
    for side in ("Left", "Right"):
        h = hands[side]
        lines.append(f"// ---- {side} hand ----")
        lines.append(f"constexpr u16 k{side}Index[{nt * 3}] = {{")
        lines.append(fmt_ints(h.g.index))
        lines.append("};")
        lines.append("")
        for kind, which, dec in (("Position", 0, 5), ("Normal", 1, 4)):
            lines.append(f"constexpr float k{side}{kind}[{len(POSE_NAMES)}][{nv * 3}] = {{")
            for pose in POSE_NAMES:
                lines.append(f"\t// {pose}")
                lines.append("\t{")
                lines.append(fmt_floats(baked[side][pose][which], dec))
                lines.append("\t},")
            lines.append("};")
            lines.append("")
        lines.append("// Per pose: the wrist joint, then the index fingertip (x, y, z each).")
        lines.append(f"constexpr float k{side}Anchor[{len(POSE_NAMES)}][6] = {{")
        for pose in POSE_NAMES:
            a = baked[side][pose][2]
            lines.append("\t{" + ", ".join(f"{v:.5f}f" for v in list(a["wrist"]) + list(a["index_tip"])) + "},  // " + pose)
        lines.append("};")
        lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def preview(path, hands, baked):
    """A contact sheet of the poses (needs matplotlib), for eyeballing them without the emulator: a column per pose, a
    row per side and view (palm side, back of the hand), the fingers pointing sideways, the pointing frame's +Y up."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection

    rows = [(side, view) for side in ("Right", "Left") for view in ("palm", "back")]
    fig, axs = plt.subplots(len(rows), len(POSE_NAMES), figsize=(4.0 * len(POSE_NAMES), 3.4 * len(rows)))
    light = unit([0.4, 0.7, 0.6])
    for r, (side, view) in enumerate(rows):
        # The palm faces -X on the right hand and +X on the left; the back is the other way. "Right" on screen is the
        # direction the viewer's right hand points, so the fingers (-Z) come out on the left for the palm view.
        face = (-1.0 if side == "Right" else 1.0) * (1.0 if view == "palm" else -1.0)
        toward = np.array([face, 0.0, 0.0])
        up = np.array([0.0, 1.0, 0.0])
        right = np.cross(up, toward)
        for c, pose in enumerate(POSE_NAMES):
            pos, nrm, _ = baked[side][pose]
            tri = pos[hands[side].g.index.reshape(-1, 3)]
            fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
            fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
            polys, cols = [], []
            for i in np.argsort(tri.mean(axis=1) @ toward):
                if fn[i] @ toward <= 0:
                    continue
                polys.append(np.stack([tri[i] @ right, tri[i] @ up], axis=1) * 100.0)
                shade = 0.35 + 0.65 * max(0.0, 0.6 * (fn[i] @ toward) + 0.4 * (fn[i] @ light))
                cols.append((0.9 * shade, 0.68 * shade, 0.55 * shade))
            ax = axs[r, c]
            ax.add_collection(PolyCollection(polys, facecolors=cols, edgecolors=(0, 0, 0, 0.25), linewidths=0.3))
            ax.set_aspect("equal")
            ax.autoscale()
            ax.grid(True, lw=0.2)
            ax.set_title(f"{side} {pose}, {view} (cm)", fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=60)
    plt.close(fig)


def check(hands, baked):
    """Sanity checks on the baked data; a problem stops the script, so a bad .inc is never written."""
    problems = []
    for side, h in hands.items():
        for pose in POSE_NAMES:
            pos, nrm, _ = baked[side][pose]
            if not (np.isfinite(pos).all() and np.isfinite(nrm).all()):
                problems.append(f"{side} {pose}: non-finite values")
            if np.abs(np.linalg.norm(nrm, axis=1) - 1.0).max() > 1e-3:
                problems.append(f"{side} {pose}: normals not unit length")
        length = baked[side]["OPEN"][0][:, 2]
        length = (length.max() - length.min()) * 100.0
        if not 17.0 <= length <= 24.0:
            problems.append(f"{side} OPEN is {length:.1f} cm long, expected about 18-21")
    if problems:
        raise SystemExit("check failed:\n  " + "\n  ".join(problems))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--left", required=True, help="Hand_low_L.gltf")
    ap.add_argument("--right", required=True, help="Hand_low_R.gltf")
    ap.add_argument("--out", required=True, help="the .inc to write (pcsx2/VR/HandMeshData.inc)")
    ap.add_argument("--preview", help="also write a contact sheet of the poses to this .png (needs matplotlib)")
    args = ap.parse_args()

    hands = {"Left": Hand(args.left, True), "Right": Hand(args.right, False)}
    for side, h in hands.items():
        if h.g.animations:
            print(f"{side}: the glTF has animations {h.g.animations}, not used: the poses are solved from POSES")
        else:
            print(f"{side}: the glTF has no animations (Godot ships the pose animations as .res/.blend); "
                  f"the poses {', '.join(POSE_NAMES)} are solved from POSES")
    baked = {side: {pose: h.bake(pose) for pose in POSE_NAMES} for side, h in hands.items()}
    check(hands, baked)
    write_inc(args.out, hands, baked)
    nv = len(hands["Right"].g.position)
    nt = len(hands["Right"].g.index) // 3
    print(f"wrote {args.out}: {nt} triangles, {nv} vertices per side, {len(POSE_NAMES)} poses, "
          f"{os.path.getsize(args.out) // 1024} KiB of source")
    for side in ("Right", "Left"):
        pos = baked[side]["OPEN"][0]
        lo, hi = pos.min(axis=0) * 100, pos.max(axis=0) * 100
        print(f"  {side} OPEN bounds (cm): x {lo[0]:.1f}..{hi[0]:.1f}  y {lo[1]:.1f}..{hi[1]:.1f}  z {lo[2]:.1f}..{hi[2]:.1f}")
    if args.preview:
        preview(args.preview, hands, baked)
        print(f"wrote {args.preview}")


if __name__ == "__main__":
    sys.exit(main())
