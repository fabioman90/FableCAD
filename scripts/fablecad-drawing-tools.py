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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("recover", "pdf", "union", "intersection", "difference"))
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--page", type=int, default=1)
    parser.add_argument("--scale", type=float, default=25.4 / 72)
    args = parser.parse_args()
    try:
        if args.operation == "recover":
            drawing, message = recover_dxf(args.source)
        elif args.operation == "pdf":
            drawing, message = pdf_paths(args.source, args.page, args.scale)
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
