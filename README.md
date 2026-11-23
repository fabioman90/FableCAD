# FableCAD

FableCAD is a 2D CAD editor built with Qt for technical drawing. Current development
focuses on macOS and Apple Silicon, with DXF support and a DWG workflow.

- [macOS, DWG and update status](docs/fablecad-macos.md)
- [FableCAD releases](https://github.com/fabioman90/FableCAD/releases)

FableCAD is based on [LibreCAD](https://github.com/LibreCAD/LibreCAD) and distributed
under the [GNU General Public License version 2](LICENSE).

## Command-Line Converters

Run `librecad <command> [options] <input files>` to convert drawings without
opening the editor:

| DXF input | DWG input | Output |
| --- | --- | --- |
| `dxf2pdf` | `dwg2pdf` | PDF document |
| `dxf2png` | `dwg2png` | PNG raster image |
| `dxf2svg` | `dwg2svg` | SVG vector image |
| `dxf2dwg` | `dwg2dxf` | DWG or DXF drawing, respectively |

DWG input and output require a build with `DWGSUPPORT` enabled.
By default, each output is saved beside its input with the same base name and
the new extension. Use `-t <directory>` to choose an output directory, or
`-o <file>` to name the output file; these options cannot be combined.
`-o` requires a single input, except for PDF conversion, where multiple inputs
are combined into one PDF.

Format-specific options include:

- PDF: `-a` fits and centers the drawing on the page; `-p <WxH>` sets paper size
  in millimeters; `-r <DPI>` sets print resolution.
- PNG: `-r <WxH>` sets image dimensions in pixels.
- DWG output: `-V <version>` selects `r2000` (default), `r2004`, `r2007`, `r2010`,
  `r2013`, or `r2018`.
- DXF output: `-V <version>` selects `r12`, `r14`, `r2000`, `r2004`, `r2007`
  (default), `r2010`, `r2013`, or `r2018`.

For example, convert a DWG drawing to a DXF R2018 file:

```bash
librecad dwg2dxf -V r2018 -o drawing.dxf drawing.dwg
```

Convert a DXF drawing to a PDF, fitted and centered on the page:

```bash
librecad dxf2pdf -a -o drawing.pdf drawing.dxf
```

Use `librecad <command> -h` for the full list of options. On systems without a
display, set `QT_QPA_PLATFORM=offscreen` for PDF, PNG, and SVG conversion.

On Windows, `LibreCAD.exe` is a GUI program: at an interactive cmd or
PowerShell prompt, and in PowerShell scripts, the prompt returns before the
conversion ends and no exit code is set, while cmd batch files wait for it. To
wait and get the exit code, use `start /wait "" LibreCAD.exe dxf2pdf ...` in
cmd (it sets `%ERRORLEVEL%`), or `Start-Process` with `-Wait -PassThru` in
PowerShell.

## Built with libdxfrw
[`libdxfrw`](https://github.com/LibreCAD/libdxfrw) is an associated project that provides DXF and DWG support.


## ESRI Shapefile (.shp) Import
FableCAD can natively import ESRI Shapefiles (`.shp`/`.shx`/`.dbf`), built with [`shapelib`](https://shapelib.maptools.org/). Points, polylines/arcs, and polygons are imported as native entities (Z preserved), with DBF-driven layers and per-entity styling (color, linetype, width) where the corresponding fields are present. Import is read-only; there is no Shapefile export.


## Documentation

The [LibreCAD manual](https://librecad.readthedocs.io/) covers tools inherited from the upstream project.

## Building

### Requirements

- [Qt](https://www.qt.io/download-open-source/) 6.4.0+ (MinGW version on Windows)
- [Boost](https://www.boost.org/) 1.55.0+

More information: [Build from source](https://github.com/LibreCAD/LibreCAD/wiki/Build-from-source)

### Plugin Installation

CMake accepts `-DLIBRECAD_PLUGIN_INSTALL_DIR=lib/librecad/plugins` to override
the plugin installation directory. Relative destinations use `CMAKE_INSTALL_PREFIX`;
absolute destinations are also accepted. The resulting absolute path is compiled
into LibreCAD and added to the runtime plugin search locations, so plugins are
found there in addition to the default locations. macOS packaging expects
plugins in `LibreCAD.app/Contents/PlugIns/LibreCAD`.

With qmake, pass an absolute destination, for example
`qmake6 librecad.pro "LIBRECAD_PLUGIN_INSTALL_DIR=/usr/lib/librecad/plugins"`.
This enables plugin installation with `make -C plugins install` without changing
the build-output directories. As with CMake, the destination is also added to
the runtime plugin search locations. Omitting the variable keeps the existing
behavior.

Pixi CI runs `pixi run check-plugin-install`, reusing the compiled sample plugin
to check default and custom destinations. qmake checks run on Linux and macOS;
Windows CI continues using CMake.

### Building Unit Tests

To build unit tests (e.g., for `rs_math.cpp`), enable the `BUILD_TESTS` flag:

```bash
cmake -DBUILD_TESTS=ON ..
make
./build/librecad_tests
```


## Upstream Development Resources

[Git and GitHub](https://github.com/LibreCAD/LibreCAD/wiki/Git-and-GitHub)

[Becoming a developer](https://github.com/LibreCAD/LibreCAD/wiki/Becoming-a-developer)

There is a [resources repository](https://github.com/LibreCAD/Resources) for people that want to indirectly  
contribute to the project by supplying icons, stylesheets, documentation, templates...

Associated downloads: <https://sourceforge.net/projects/librecad/files/Resources/>
