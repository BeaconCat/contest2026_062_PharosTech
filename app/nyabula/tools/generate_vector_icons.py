#!/usr/bin/env python3

"""Flatten the approved Nyabula SVG assets into deterministic C contours."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from fontTools.pens.recordingPen import RecordingPen
from fontTools.svgLib.path import parse_path


def sample_line(start, end):
    return [end]


def sample_cubic(start, p1, p2, end, steps=10):
    points = []
    for index in range(1, steps + 1):
        t = index / steps
        u = 1.0 - t
        points.append((u**3 * start[0] + 3 * u * u * t * p1[0] +
                       3 * u * t * t * p2[0] + t**3 * end[0],
                       u**3 * start[1] + 3 * u * u * t * p1[1] +
                       3 * u * t * t * p2[1] + t**3 * end[1]))
    return points


def sample_quadratic(start, control, end, steps=8):
    points = []
    for index in range(1, steps + 1):
        t = index / steps
        u = 1.0 - t
        points.append((u * u * start[0] + 2 * u * t * control[0] +
                       t * t * end[0],
                       u * u * start[1] + 2 * u * t * control[1] +
                       t * t * end[1]))
    return points


def flatten(path_data):
    pen = RecordingPen()
    parse_path(path_data, pen)
    contours = []
    contour = []
    current = (0.0, 0.0)
    origin = current
    for command, args in pen.value:
        if command == "moveTo":
            if contour:
                contours.append(contour)
            current = args[0]
            origin = current
            contour = [current]
        elif command == "lineTo":
            contour.extend(sample_line(current, args[0]))
            current = args[0]
        elif command == "curveTo":
            contour.extend(sample_cubic(current, args[0], args[1], args[2]))
            current = args[2]
        elif command == "qCurveTo":
            points = list(args)
            end = points.pop()
            while len(points) > 1:
                implied = ((points[0][0] + points[1][0]) * 0.5,
                           (points[0][1] + points[1][1]) * 0.5)
                contour.extend(sample_quadratic(current, points[0], implied))
                current = implied
                points.pop(0)
            contour.extend(sample_quadratic(current, points[0], end))
            current = end
        elif command in ("closePath", "endPath"):
            if command == "closePath" and current != origin:
                contour.append(origin)
            if contour:
                contours.append(contour)
                contour = []
            current = origin
        else:
            raise ValueError(f"unsupported SVG command: {command}")
    if contour:
        contours.append(contour)
    return contours


def read_svg(path):
    text = path.read_text(encoding="utf-8")
    viewbox = re.search(r'viewBox="([^"]+)"', text)
    path_data = re.search(r'<path[^>]+d="([^"]+)"', text)
    if not viewbox or not path_data:
        raise ValueError(f"missing viewBox/path in {path}")
    values = [float(value) for value in viewbox.group(1).split()]
    return values, flatten(path_data.group(1))


def read_icon_map(path):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"Object\.freeze\((\{.*\})\);", text, re.DOTALL)
    if match is None:
        raise ValueError(f"missing icon map in {path}")
    source = json.loads(match.group(1))
    return [
        (ident(name), icon["viewBox"], flatten(icon["path"]))
        for name, icon in sorted(source.items())
    ]


def ident(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def render(source, output_h, output_c):
    if source.is_file():
        icons = read_icon_map(source)
    else:
        icons = []
        for path in sorted(source.glob("*.svg")):
            viewbox, contours = read_svg(path)
            icons.append((ident(path.stem), viewbox, contours))

    header = """/****************************************************************************
 * app/nyabula/src/generated/nyabula_eye_icons.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#ifndef __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H
#define __APP_NYABULA_SRC_GENERATED_NYABULA_EYE_ICONS_H

#include <stdint.h>

struct nyabula_eye_icon_point_s
{
  int32_t x;
  int32_t y;
};

struct nyabula_eye_icon_s
{
  const char *name;
  const struct nyabula_eye_icon_point_s *points;
  const uint16_t *contours;
  uint16_t point_count;
  uint16_t contour_count;
  uint32_t width;
  uint32_t height;
};

const struct nyabula_eye_icon_s *nyabula_eye_icon_find(const char *name);

#endif
"""
    output_h.parent.mkdir(parents=True, exist_ok=True)
    output_h.write_text(header, encoding="utf-8", newline="\n")

    body = ["""/****************************************************************************
 * app/nyabula/src/generated/nyabula_eye_icons.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#include <stddef.h>
#include <string.h>

#include \"nyabula_eye_icons.h\"
"""]
    entries = []
    coordinate_width = 4096
    for name, viewbox, contours in icons:
        points = []
        offsets = [0]
        coordinate_height = round(viewbox[3] / viewbox[2] * coordinate_width)
        for contour in contours:
            for x, y in contour:
                points.append(
                    (
                        round((x - viewbox[0]) / viewbox[2] * coordinate_width),
                        round((y - viewbox[1]) / viewbox[3] * coordinate_height),
                    )
                )
            offsets.append(len(points))
        point_text = ",\n  ".join(f"{{{x}, {y}}}" for x, y in points)
        offset_text = ", ".join(str(value) for value in offsets)
        body.append(f"\nstatic const struct nyabula_eye_icon_point_s g_{name}_points[] =\n{{\n  {point_text}\n}};\n")
        body.append(f"static const uint16_t g_{name}_contours[] =\n{{\n  {offset_text}\n}};\n")
        entries.append(
            f'  {{"{name}", g_{name}_points, g_{name}_contours, '
            f'{len(points)}, {len(contours)}, {coordinate_width}, '
            f'{coordinate_height}}}')
    body.append("\nstatic const struct nyabula_eye_icon_s g_icons[] =\n{\n" +
                ",\n".join(entries) + "\n};\n")
    body.append("""
const struct nyabula_eye_icon_s *nyabula_eye_icon_find(const char *name)
{
  size_t index;

  for (index = 0; index < sizeof(g_icons) / sizeof(g_icons[0]); index++)
    {
      if (strcmp(g_icons[index].name, name) == 0)
        {
          return &g_icons[index];
        }
    }

  return NULL;
}
""")
    output_c.write_text("".join(body), encoding="utf-8", newline="\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("header", type=Path)
    parser.add_argument("source_output", type=Path)
    args = parser.parse_args()
    render(args.source, args.header, args.source_output)


if __name__ == "__main__":
    main()
