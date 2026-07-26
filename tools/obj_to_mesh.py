#!/usr/bin/env python3
# tools/obj_to_mesh.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.

"""Convert a Wavefront OBJ model into the compact SoftGL .mesh container.

The on-disk layout matches struct softgl_mesh_header_s in
app/softgl/softgl.h exactly (little endian, no padding):

    offset  size  field
    0       4     magic      'SGLM' (0x4d4c4753)
    4       4     version    1
    8       4     nvertices
    12      4     nindices   (multiple of 3)
    16      12    bbox_min[3] float
    28      12    bbox_max[3] float
    40      32*n  vertices: px py pz nx ny nz u v  (float32)
    ...     2*m   indices: uint16

Because indices are 16-bit, a mesh may not exceed 65535 unique vertices --
which is far above what the CPU rasteriser can push at interactive rates
anyway.  Use --decimate-check to be warned early about heavy models.

Typical use for the Nyabula cat-house assets:

    ./obj_to_mesh.py cat.obj cat.mesh --normalize --scale 1.0 --smooth
    ./obj_to_mesh.py cat.obj --c-header cat_mesh.h --symbol g_cat_mesh
"""

import argparse
import math
import struct
import sys

SOFTGL_MESH_MAGIC = 0x4D4C4753
SOFTGL_MESH_VERSION = 1
SOFTGL_MAX_VERTICES = 65535


def parse_obj(path):
    """Read an OBJ file into positions, texcoords, normals and faces.

    Returns (positions, texcoords, normals, faces) where each face is a list
    of (v, vt, vn) zero-based indices; vt/vn are None when absent.
    """
    positions = []
    texcoords = []
    normals = []
    faces = []

    def fix(index, count):
        """OBJ indices are 1-based; negative values count back from the end."""
        if index > 0:
            return index - 1
        if index < 0:
            return count + index
        return None

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split()
            tag = parts[0]

            if tag == "v" and len(parts) >= 4:
                positions.append(tuple(float(v) for v in parts[1:4]))
            elif tag == "vn" and len(parts) >= 4:
                normals.append(tuple(float(v) for v in parts[1:4]))
            elif tag == "vt" and len(parts) >= 3:
                texcoords.append((float(parts[1]), float(parts[2])))
            elif tag == "f" and len(parts) >= 4:
                corners = []
                for token in parts[1:]:
                    fields = token.split("/")
                    try:
                        vi = fix(int(fields[0]), len(positions))
                    except ValueError:
                        raise SystemExit(
                            "%s:%d: malformed face token %r"
                            % (path, lineno, token))

                    ti = None
                    ni = None
                    if len(fields) > 1 and fields[1]:
                        ti = fix(int(fields[1]), len(texcoords))
                    if len(fields) > 2 and fields[2]:
                        ni = fix(int(fields[2]), len(normals))

                    corners.append((vi, ti, ni))

                # Triangulate as a fan; OBJ polygons are convex by
                # convention.
                for k in range(1, len(corners) - 1):
                    faces.append([corners[0], corners[k], corners[k + 1]])

    return positions, texcoords, normals, faces


def build_vertices(positions, texcoords, normals, faces, flip_v):
    """Emit one unique vertex per distinct v/vt/vn triple."""
    lookup = {}
    verts = []
    indices = []

    for face in faces:
        for key in face:
            slot = lookup.get(key)
            if slot is None:
                vi, ti, ni = key
                pos = positions[vi]
                uv = texcoords[ti] if ti is not None and ti < len(texcoords) \
                    else (0.0, 0.0)
                nrm = normals[ni] if ni is not None and ni < len(normals) \
                    else (0.0, 0.0, 0.0)

                if flip_v:
                    uv = (uv[0], 1.0 - uv[1])

                slot = len(verts)
                if slot >= SOFTGL_MAX_VERTICES:
                    raise SystemExit(
                        "error: more than %d unique vertices; simplify the "
                        "model (SoftGL uses 16-bit indices)"
                        % SOFTGL_MAX_VERTICES)

                verts.append([pos[0], pos[1], pos[2],
                              nrm[0], nrm[1], nrm[2],
                              uv[0], uv[1]])
                lookup[key] = slot

            indices.append(slot)

    return verts, indices


def compute_normals(verts, indices, smooth):
    """Replace the normals with generated ones.

    smooth=True area-weight averages the face normals per vertex; smooth=False
    splits every triangle into its own three vertices first, giving hard
    edges (the look the low-poly cat-house assets want).
    """
    if not smooth:
        split_verts = []
        split_indices = []
        for i in range(0, len(indices), 3):
            base = len(split_verts)
            for k in range(3):
                split_verts.append(list(verts[indices[i + k]]))
            split_indices.extend([base, base + 1, base + 2])

        if len(split_verts) > SOFTGL_MAX_VERTICES:
            raise SystemExit(
                "error: flat shading needs %d vertices, over the 65535 limit; "
                "use --smooth or simplify the model" % len(split_verts))

        verts = split_verts
        indices = split_indices

    for vert in verts:
        vert[3] = vert[4] = vert[5] = 0.0

    for i in range(0, len(indices), 3):
        a = verts[indices[i]]
        b = verts[indices[i + 1]]
        c = verts[indices[i + 2]]

        ab = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        ac = (c[0] - a[0], c[1] - a[1], c[2] - a[2])

        # Un-normalised cross product is area weighted.
        nx = ab[1] * ac[2] - ab[2] * ac[1]
        ny = ab[2] * ac[0] - ab[0] * ac[2]
        nz = ab[0] * ac[1] - ab[1] * ac[0]

        for vert in (a, b, c):
            vert[3] += nx
            vert[4] += ny
            vert[5] += nz

    for vert in verts:
        length = math.sqrt(vert[3] ** 2 + vert[4] ** 2 + vert[5] ** 2)
        if length > 1e-12:
            vert[3] /= length
            vert[4] /= length
            vert[5] /= length
        else:
            vert[3], vert[4], vert[5] = 0.0, 1.0, 0.0

    return verts, indices


def transform(verts, normalize, scale, center):
    """Optionally recentre and rescale the model in place."""
    if not verts:
        raise SystemExit("error: model has no vertices")

    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]

    offset = [0.0, 0.0, 0.0]
    if center or normalize:
        offset = [(lo[i] + hi[i]) * 0.5 for i in range(3)]

    factor = scale
    if normalize:
        extent = max(hi[i] - lo[i] for i in range(3))
        if extent > 1e-12:
            factor = scale / extent

    if offset != [0.0, 0.0, 0.0] or factor != 1.0:
        for vert in verts:
            for i in range(3):
                vert[i] = (vert[i] - offset[i]) * factor

        lo = [min(v[i] for v in verts) for i in range(3)]
        hi = [max(v[i] for v in verts) for i in range(3)]

    return lo, hi


def pack_mesh(verts, indices, lo, hi):
    """Serialise to the .mesh byte string."""
    out = bytearray()
    out += struct.pack("<IIII", SOFTGL_MESH_MAGIC, SOFTGL_MESH_VERSION,
                       len(verts), len(indices))
    out += struct.pack("<3f", *lo)
    out += struct.pack("<3f", *hi)

    for vert in verts:
        out += struct.pack("<8f", *vert)

    for index in indices:
        out += struct.pack("<H", index)

    return bytes(out)


def write_c_header(path, symbol, blob):
    """Emit the container as a C array so it can be linked into the image."""
    guard = "__" + symbol.upper() + "_H"

    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            "/* Generated by tools/obj_to_mesh.py -- do not edit. */\n\n"
            "#ifndef %s\n#define %s\n\n"
            "#include <stddef.h>\n#include <stdint.h>\n\n"
            "static const uint8_t %s[] =\n{\n" % (guard, guard, symbol))

        for offset in range(0, len(blob), 12):
            chunk = blob[offset:offset + 12]
            handle.write("  " + " ".join("0x%02x," % b for b in chunk) + "\n")

        handle.write("};\n\n"
                     "#define %s_SIZE %uu\n\n"
                     "#endif /* %s */\n"
                     % (symbol.upper(), len(blob), guard))


def main(argv):
    parser = argparse.ArgumentParser(
        description="Convert a Wavefront OBJ into a SoftGL .mesh container.")
    parser.add_argument("input", help="input .obj file")
    parser.add_argument("output", nargs="?",
                        help="output .mesh file (omit when using --c-header)")
    parser.add_argument("--c-header", metavar="FILE",
                        help="also emit the blob as a C byte array")
    parser.add_argument("--symbol", default="g_softgl_mesh",
                        help="C array name for --c-header")
    parser.add_argument("--scale", type=float, default=1.0,
                        help="uniform scale factor (default 1.0)")
    parser.add_argument("--normalize", action="store_true",
                        help="fit the model into a --scale sized cube")
    parser.add_argument("--center", action="store_true",
                        help="recentre the model on its bounding box")
    parser.add_argument("--smooth", action="store_true",
                        help="regenerate smooth (averaged) normals")
    parser.add_argument("--flat", action="store_true",
                        help="regenerate hard per-face normals")
    parser.add_argument("--keep-v", action="store_true",
                        help="keep the OBJ V axis instead of flipping it "
                             "(SoftGL samples textures top-down)")
    parser.add_argument("--decimate-check", type=int, default=0,
                        metavar="TRIS",
                        help="warn if the result exceeds TRIS triangles")
    args = parser.parse_args(argv[1:])

    if args.smooth and args.flat:
        parser.error("--smooth and --flat are mutually exclusive")

    if not args.output and not args.c_header:
        parser.error("give an output .mesh path, a --c-header, or both")

    positions, texcoords, normals, faces = parse_obj(args.input)
    if not faces:
        raise SystemExit("error: %s contains no faces" % args.input)

    verts, indices = build_vertices(positions, texcoords, normals, faces,
                                    not args.keep_v)

    if args.smooth or args.flat or not normals:
        verts, indices = compute_normals(verts, indices, not args.flat)

    lo, hi = transform(verts, args.normalize, args.scale, args.center)
    blob = pack_mesh(verts, indices, lo, hi)

    ntris = len(indices) // 3
    if args.decimate_check and ntris > args.decimate_check:
        print("warning: %d triangles exceeds the %d triangle budget"
              % (ntris, args.decimate_check), file=sys.stderr)

    if args.output:
        with open(args.output, "wb") as handle:
            handle.write(blob)

    if args.c_header:
        write_c_header(args.c_header, args.symbol, blob)

    print("%s: %d vertices, %d triangles, %d bytes"
          % (args.input, len(verts), ntris, len(blob)))
    print("  bbox min (%.4f, %.4f, %.4f)  max (%.4f, %.4f, %.4f)"
          % (lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
