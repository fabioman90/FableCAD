#!/usr/bin/env python3
"""Optional local PDF, DXF and Clipper2 tools for FableCAD."""
import argparse
import ctypes
from contextlib import closing
import math
import os
from pathlib import Path
import tempfile


def recover_dxf(source):
    from ezdxf import recover

    drawing, audit = recover.readfile(source)
    if audit.errors:
        raise ValueError(f"{len(audit.errors)} DXF errors could not be repaired.")
    return drawing, f"Recovered drawing. Audit fixes: {len(audit.fixes)}."


def pdf_paths(source, page_number, scale):
    import ezdxf
    import pypdfium2 as pdfium
    from pypdfium2 import raw

    if not math.isfinite(scale) or scale <= 0:
        raise ValueError("Scale must be a positive finite number.")
    drawing = ezdxf.new("R2018")
    drawing.units = ezdxf.units.MM
    model = drawing.modelspace()
    skipped = 0
    text_count = 0
    skipped_text = 0

    with pdfium.PdfDocument(source) as document:
        if not 1 <= page_number <= len(document):
            raise ValueError(f"Page must be between 1 and {len(document)}.")
        page = document[page_number - 1]
        for obj in page.get_objects(filter=[raw.FPDF_PAGEOBJ_PATH]):
            if obj.container is not None:
                skipped += 1
                continue
            matrix = obj.get_matrix()
            paths = []
            segments = []
            start = None
            for index in range(raw.FPDFPath_CountSegments(obj)):
                segment = raw.FPDFPath_GetPathSegment(obj, index)
                kind = raw.FPDFPathSegment_GetType(segment)
                x, y = ctypes.c_float(), ctypes.c_float()
                if not raw.FPDFPathSegment_GetPoint(segment, x, y):
                    break
                px, py = matrix.on_point(x.value, y.value)
                point = (px * scale, py * scale)
                if kind == raw.FPDF_SEGMENT_MOVETO:
                    if segments:
                        paths.append((start, segments, False))
                    start, segments = point, []
                elif start is not None and kind in (raw.FPDF_SEGMENT_LINETO, raw.FPDF_SEGMENT_BEZIERTO):
                    segments.append((kind, point))
                else:
                    break
                if raw.FPDFPathSegment_GetClose(segment):
                    paths.append((start, segments, True))
                    segments = []
            else:
                if segments:
                    paths.append((start, segments, False))
                for start, segments, closed in paths:
                    emit_pdf_path(model, start, segments, closed, raw)
                continue
            skipped += 1
        with closing(page.get_textpage()) as textpage:
            for obj in page.get_objects(filter=[raw.FPDF_PAGEOBJ_TEXT], textpage=textpage):
                if obj.container is not None:
                    skipped_text += 1
                    continue
                try:
                    imported = emit_pdf_text(model, obj, scale)
                except (pdfium.PdfiumError, UnicodeError):
                    imported = False
                text_count += imported
                skipped_text += not imported
    if not len(model):
        raise ValueError("The PDF page contains no supported vector paths or text.")
    return drawing, (f"Imported {len(model)} entities, including {text_count} text objects. "
                     f"Skipped {skipped} paths and {skipped_text} text objects.")


def emit_pdf_text(model, obj, scale):
    text = obj.extract()
    if not text.strip() or any(ord(char) < 32 or char == "\ufffd" for char in text):
        return False

    matrix = obj.get_matrix()
    x_scale = math.hypot(matrix.a, matrix.b)
    y_scale = math.hypot(matrix.c, matrix.d)
    if not all(math.isfinite(value) for value in (x_scale, y_scale, matrix.e, matrix.f)):
        return False
    if not x_scale or not y_scale or matrix.a * matrix.d - matrix.b * matrix.c <= 0:
        return False
    # DXF TEXT uses a single baseline and width factor. Skip sheared transforms.
    if abs(matrix.a * matrix.c + matrix.b * matrix.d) > 1e-5 * x_scale * y_scale:
        return False

    # PDF font size describes the em square, while DXF text height describes caps.
    # A fallback CAD style approximates glyph size when the PDF font is unavailable.
    height = obj.get_font_size() * y_scale * scale * 0.7
    if not math.isfinite(height) or height <= 0:
        return False
    model.add_text(text, dxfattribs={
        "insert": (matrix.e * scale, matrix.f * scale),
        "height": height,
        "rotation": math.degrees(math.atan2(matrix.b, matrix.a)) % 360,
        "width": x_scale / y_scale,
    })
    return True


def emit_pdf_path(model, start, segments, closed, raw):
    if all(kind == raw.FPDF_SEGMENT_LINETO for kind, _ in segments):
        vertices = [start, *(point for _, point in segments)]
        if len(vertices) == 2 and not closed:
            model.add_line(*vertices)
        elif len(vertices) >= 3:
            model.add_lwpolyline(vertices, close=closed)
        return

    current = start
    controls = []
    for kind, point in segments:
        if kind == raw.FPDF_SEGMENT_BEZIERTO:
            controls.append(point)
            if len(controls) == 3:
                model.add_open_spline([current, *controls], degree=3)
                current = point
                controls.clear()
        else:
            if controls:
                raise ValueError("Invalid cubic Bézier segment in the PDF path.")
            model.add_line(current, point)
            current = point
    if controls:
        raise ValueError("Incomplete cubic Bézier segment in the PDF path.")
    if closed and current != start:
        model.add_line(current, start)


def combine_contours(source, operation):
    import ezdxf
    import numpy as np
    import pyclipr

    original = ezdxf.readfile(source)
    contours = list(original.modelspace().query("LWPOLYLINE"))
    if len(contours) != 2 or len(original.modelspace()) != 2:
        raise ValueError("Provide a DXF containing exactly two closed lightweight polylines.")

    clipper = pyclipr.Clipper()
    clipper.scaleFactor = 1_000_000
    for contour, role in zip(contours, (pyclipr.Subject, pyclipr.Clip)):
        vertices = list(contour.get_points("xyb"))
        if (not contour.closed or len(vertices) < 3 or contour.dxf.elevation != 0
                or tuple(contour.dxf.extrusion) != (0, 0, 1)
                or any(bulge or not math.isfinite(x) or not math.isfinite(y)
                       or max(abs(x), abs(y)) > 1e9 for x, y, bulge in vertices)):
            raise ValueError("Contours must be closed, planar, straight-edged 2D polylines.")
        clipper.addPath(np.array([(x, y) for x, y, _ in vertices]), role)
    kind = {"union": pyclipr.Union, "intersection": pyclipr.Intersection,
            "difference": pyclipr.Difference}[operation]
    result = clipper.execute(kind, pyclipr.FillRule.EvenOdd)
    if not len(result):
        raise ValueError("The selected operation produced no contours.")
    drawing = ezdxf.new("R2018")
    drawing.units = original.units
    for contour in result:
        drawing.modelspace().add_lwpolyline(contour, close=True)
    return drawing, f"Created {len(result)} closed contours."


def vectorize_image(source, reference_pixels, reference_mm):
    import re
    from xml.etree import ElementTree

    import ezdxf
    import vtracer
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.svgLib.path import parse_path

    if source.suffix.lower() not in {".png", ".jpg", ".jpeg"}:
        raise ValueError("Choose a PNG or JPEG image.")
    if not all(math.isfinite(value) and value > 0 for value in (reference_pixels, reference_mm)):
        raise ValueError("Both reference lengths must be positive finite numbers.")
    scale = reference_mm / reference_pixels
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError("The reference lengths produce an invalid scale.")

    with tempfile.TemporaryDirectory(prefix="fablecad-trace-") as staging:
        svg = Path(staging) / "trace.svg"
        vtracer.convert_image_to_svg_py(str(source), str(svg), colormode="binary", mode="polygon")
        traced = ElementTree.parse(svg).getroot()

    drawing = ezdxf.new("R2018")
    drawing.units = ezdxf.units.MM
    drawing.layers.new("RASTER_TRACE")
    model = drawing.modelspace()
    image_height = float(traced.attrib["height"])
    paths = "{http://www.w3.org/2000/svg}path"
    for path in traced.findall(paths):
        dx = dy = 0.0
        if transform := path.get("transform"):
            offset = re.fullmatch(r"translate\(\s*([^,\s]+)[,\s]+([^,\s]+)\s*\)", transform)
            if not offset:
                raise ValueError("The vectorizer returned an unsupported path transform.")
            dx, dy = map(float, offset.groups())

        pen = RecordingPen()
        parse_path(path.attrib["d"], pen)
        points = []
        for operation, operands in pen.value:
            if operation in {"moveTo", "lineTo"}:
                x, y = operands[0]
                point = ((x + dx) * scale, (image_height - y - dy) * scale)
                if not all(map(math.isfinite, point)):
                    raise ValueError("The calibrated contour exceeds the numeric range.")
                points.append(point)
            elif operation == "closePath":
                if points and points[-1] == points[0]:
                    points.pop()
                if len(points) >= 3:
                    model.add_lwpolyline(points, close=True, dxfattribs={"layer": "RASTER_TRACE"})
                points = []
            else:
                raise ValueError("The vectorizer returned an unsupported path segment.")
    if not len(model):
        raise ValueError("No dark contours were found in the image.")
    return drawing, f"Traced {len(model)} contours at {scale:.6g} mm per pixel."


def split_free_rectangles(free_rectangles, placed):
    left, bottom, right, top = placed
    split = set()
    for x0, y0, x1, y1 in free_rectangles:
        if right <= x0 or x1 <= left or top <= y0 or y1 <= bottom:
            split.add((x0, y0, x1, y1))
            continue
        if x0 < left:
            split.add((x0, y0, left, y1))
        if right < x1:
            split.add((right, y0, x1, y1))
        if y0 < bottom:
            split.add((x0, y0, x1, bottom))
        if top < y1:
            split.add((x0, top, x1, y1))
    return sorted(rect for rect in split if not any(
        other != rect and other[0] <= rect[0] and other[1] <= rect[1]
        and other[2] >= rect[2] and other[3] >= rect[3] for other in split
    ))


def nest_sheet(source, sheet_width, sheet_height, gap, rotate):
    import ezdxf

    if not all(math.isfinite(value) for value in (sheet_width, sheet_height, gap)):
        raise ValueError("Sheet dimensions and clearance must be finite.")
    if gap < 0 or min(sheet_width, sheet_height) <= 2 * gap:
        raise ValueError("The sheet must have positive space inside its edge clearance.")
    original = ezdxf.readfile(source)
    parts = []
    for index, entity in enumerate(original.modelspace(), 1):
        if (entity.dxftype() != "LWPOLYLINE" or not entity.closed
                or entity.dxf.elevation != 0 or tuple(entity.dxf.extrusion) != (0, 0, 1)
                or entity.dxf.const_width):
            raise ValueError(f"Part {index} must be a closed, planar 2D lightweight polyline with zero width.")
        points = list(entity.get_points("xyseb"))
        if (len(points) < 3 or any(sw or ew or bulge or not math.isfinite(x) or not math.isfinite(y)
                                  for x, y, sw, ew, bulge in points)):
            raise ValueError(f"Part {index} must have at least three finite, straight-edged vertices and zero width.")
        x0 = min(p[0] for p in points)
        y0 = min(p[1] for p in points)
        vertices = [(x - x0, y - y0) for x, y, *_ in points]
        width = max(x for x, _ in vertices)
        height = max(y for _, y in vertices)
        area = abs(sum(x * next_y - y * next_x
                       for (x, y), (next_x, next_y) in zip(vertices, vertices[1:] + vertices[:1]))) / 2
        if not math.isfinite(area) or area == 0:
            raise ValueError(f"Part {index} has no usable area.")
        parts.append((index, width, height, area, vertices))
    if not parts:
        raise ValueError("The source DXF contains no parts.")

    # Reserve clearance on the right/top of each bounding box. The outer
    # rectangle leaves the same clearance between the profiles and the sheet.
    free_rectangles = [(gap, gap, sheet_width, sheet_height)]
    drawing = ezdxf.new("R2018")
    drawing.units = original.units
    drawing.layers.new("NEST_PARTS")
    drawing.layers.new("NEST_SHEET", dxfattribs={"color": 8})
    output = drawing.modelspace()
    rotated_count = 0
    for index, width, height, _, vertices in sorted(
            parts, key=lambda part: (-max(part[1:3]), -min(part[1:3]), part[0])):
        sizes = [(width + gap, height + gap, False)]
        if rotate and width != height:
            sizes.append((height + gap, width + gap, True))
        candidates = []
        for x0, y0, x1, y1 in free_rectangles:
            for w, h, rotated in sizes:
                dx, dy = x1 - x0 - w, y1 - y0 - h
                if dx >= 0 and dy >= 0:
                    candidates.append((min(dx, dy), max(dx, dy), y0, x0, rotated, w, h))
        if not candidates:
            raise ValueError(f"Part {index} could not be placed. Try a larger sheet, a smaller clearance, or rotation.")
        _, _, y, x, rotated, w, h = min(candidates)
        free_rectangles = split_free_rectangles(free_rectangles, (x, y, x + w, y + h))
        if rotated:
            vertices = [(height - vy, vx) for vx, vy in vertices]
            rotated_count += 1
        output.add_lwpolyline([(x + vx, y + vy) for vx, vy in vertices], close=True,
                              dxfattribs={"layer": "NEST_PARTS"})
    output.add_lwpolyline([(0, 0), (sheet_width, 0), (sheet_width, sheet_height), (0, sheet_height)],
                          close=True, dxfattribs={"layer": "NEST_SHEET"})
    utilization = sum(part[3] for part in parts) / sheet_width / sheet_height
    return drawing, (f"Placed {len(parts)} parts ({rotated_count} rotated). "
                     f"Profiles cover {utilization:.1%} of the sheet.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("recover", "pdf", "union", "intersection", "difference", "nest", "trace"))
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--page", type=int, default=1)
    parser.add_argument("--scale", type=float, default=25.4 / 72)
    parser.add_argument("--sheet-width", type=float, default=1000)
    parser.add_argument("--sheet-height", type=float, default=500)
    parser.add_argument("--gap", type=float, default=5)
    parser.add_argument("--rotate", action="store_true")
    parser.add_argument("--reference-pixels", type=float)
    parser.add_argument("--reference-mm", type=float)
    args = parser.parse_args()
    if args.operation == "trace" and (args.reference_pixels is None or args.reference_mm is None):
        parser.error("trace requires --reference-pixels and --reference-mm")
    try:
        if args.operation == "recover":
            drawing, message = recover_dxf(args.source)
        elif args.operation == "pdf":
            drawing, message = pdf_paths(args.source, args.page, args.scale)
        elif args.operation == "trace":
            drawing, message = vectorize_image(args.source, args.reference_pixels, args.reference_mm)
        elif args.operation == "nest":
            drawing, message = nest_sheet(args.source, args.sheet_width, args.sheet_height, args.gap, args.rotate)
        else:
            drawing, message = combine_contours(args.source, args.operation)
        import ezdxf

        with tempfile.TemporaryDirectory(dir=args.destination.parent, prefix=".fablecad-") as staging:
            candidate = Path(staging) / "drawing.dxf"
            drawing.saveas(candidate)
            ezdxf.readfile(candidate)
            os.link(candidate, args.destination)
    except Exception as error:
        parser.exit(1, f"{error}\n")
    print(message)


if __name__ == "__main__":
    main()
