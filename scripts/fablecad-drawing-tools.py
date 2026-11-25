#!/usr/bin/env python3
"""Optional local PDF, DXF and Clipper2 tools for FableCAD."""
import argparse
import ctypes
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
            points = []
            unsupported = False
            closed = False
            segment_count = raw.FPDFPath_CountSegments(obj)
            for index in range(segment_count):
                segment = raw.FPDFPath_GetPathSegment(obj, index)
                kind = raw.FPDFPathSegment_GetType(segment)
                if kind == raw.FPDF_SEGMENT_MOVETO and points:
                    paths.append((points, closed))
                    points = []
                    closed = False
                if kind not in (raw.FPDF_SEGMENT_MOVETO, raw.FPDF_SEGMENT_LINETO):
                    unsupported = True
                    break
                x, y = ctypes.c_float(), ctypes.c_float()
                if not raw.FPDFPathSegment_GetPoint(segment, x, y):
                    unsupported = True
                    break
                px, py = matrix.on_point(x.value, y.value)
                points.append((px * scale, py * scale))
                closed = bool(raw.FPDFPathSegment_GetClose(segment))
            if unsupported:
                skipped += 1
                continue
            if points:
                paths.append((points, closed))
            for vertices, is_closed in paths:
                if len(vertices) == 2 and not is_closed:
                    model.add_line(*vertices)
                elif len(vertices) >= 3:
                    model.add_lwpolyline(vertices, close=is_closed)
    if not len(model):
        raise ValueError("The PDF page contains no supported vector paths.")
    return drawing, f"Imported {len(model)} entities. Skipped {skipped} curved or nested paths."


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
