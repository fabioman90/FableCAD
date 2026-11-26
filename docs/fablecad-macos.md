# FableCAD on macOS

## Interface and packaging

The project uses the FableCAD name, logo, and icons. This update covers the toolbars, commands, work panels, Italian localization, and macOS bundle configuration. The ARM64 DMG for version 2.2.9 was signed, notarized by Apple, and tested on the development Mac.

## Opening and saving DWG files

The experimental bridge uses acadrust 0.6.8. Build it with `libraries/acadrust_bridge/build-experimental.sh` and set `FABLECAD_ACADRUST_HELPER` to the executable path.

The bridge retains the source DWG document and verifies the temporary output before replacing the destination. Tests covered 2D geometry, text, dimensions, layers, simple blocks, and solid and patterned hatches. The samples retained dynamic block definitions and the opaque DATATABLE data in a DWG 2004 file.

AutoCAD 2020 uses DWG 2018. Sample drawings were checked with LibreDWG and ODA. On Apple Silicon, the editor's open and save workflow was exercised with five drawings. Each was edited, saved as DWG 2018, reopened, and edited again. LibreDWG confirmed both edits, and input hashes remained unchanged. Direct testing in AutoCAD 2020 is pending.

## Updates

The release checker can notify users of newer GitHub releases at startup. Use **Help → Check for Updates** to start a check manually. On macOS, Sparkle 2 can install a differential update when the feed provides a patch for the installed version.

Set the optional CMake variables `FABLECAD_SPARKLE_FRAMEWORK`, `FABLECAD_SPARKLE_FEED_URL`, and `FABLECAD_SPARKLE_PUBLIC_KEY` to the framework path, HTTPS appcast URL, and public EdDSA key.

A local HTTPS test with two Developer ID-signed builds verified the update alert, the 97,846-byte delta download, installation, and relaunch into 2.2.10. The full ZIP was 115,815,311 bytes. A separate local test rejected a modified patch. The [2.2.10 release](https://github.com/fabioman90/FableCAD/releases/tag/v2.2.10) is the first Apple Silicon release configured for Sparkle. Its Developer ID-signed DMG and app were notarized and stapled. Users of the distributed 2.2.9 installer need to install 2.2.10 manually. Later versions are offered through the [signed production appcast](https://github.com/fabioman90/FableCAD/releases/latest/download/appcast.xml). An updater-enabled 2.2.9 test copy verified the alert, installation, and relaunch into the public 2.2.10 build. The production Sparkle key is held in the macOS Keychain.

### Annual editions

An edition purchase is attached to the customer account. The customer can download
the purchased edition again after moving to another Mac. The installed editor
works offline and the next annual edition is an optional new purchase.

Set `FABLECAD_EDITION_YEAR=2027` for the first commercial macOS edition. The year
appears in the app display name and About window. The technical version remains
available for identifying individual updates. Supply a Sparkle feed
ending in `/YEAR/appcast.xml` and use a separate signing key for each edition.
Publish only that year's updates and deltas in its signed feed. Annual builds
check their edition's Sparkle feed exclusively. Leave the edition year empty
for builds that use the existing release channel.

## Drawing tools

**File → Export → Export Scaled PDF** opens Print Preview. Use the paper settings
and scale field, then use **Center Area** to place the page over a detail by
picking opposite corners. The chosen corners position the paper at the active
scale. The printable paper margins define the clipping boundary. Use **Export
PDF** in the preview to save the vector output.

**File → Drawing Tools → DWG Diagnostics** reports model-space entity types with
editing restrictions when the drawing was opened with Acadrust. **Save
Verified Copy** writes a separate DWG 2018 file and checks that it reopens.
The active drawing remains associated with its original file.

In **Library Browser**, search across installed and custom block folders by
file name. The results show previews and identify their folders. Double-click a
result to start placement, then set the scale and rotation with the existing
insertion options.

**Options → Light Interface Theme** selects the light palette for the built-in
interface. Drawing background colours and custom stylesheets are configured
through the existing appearance settings.

**File → Drawing Recovery** lists autosaves recorded by the editor. It opens
a separate DXF copy at a new path. The same list is offered at startup when
autosaves are available. DXF saves and autosaves are staged and reopened
before replacing their destination.

The optional **File → Drawing Tools** menu calls a local helper. macOS packages can place
a standalone helper in `Contents/Helpers/fablecad-drawing-tools`. For development, set
`FABLECAD_DRAWING_TOOLS` to an absolute path to an executable that runs
`scripts/fablecad-drawing-tools.py` with Python and the `ezdxf`,
`pypdfium2`, and `pyclipr` packages available. If you use a virtual
environment, the executable wrapper should invoke that environment's Python
and forward all arguments. The helper works offline.

- **Import Vector PDF** imports lines and cubic Bézier paths from a selected
  page at a scale in millimetres per PDF point. Bézier curves, including arcs
  represented as curves in a PDF, become editable DXF splines. Selectable
  text becomes editable DXF TEXT with its baseline position and rotation.
  PDF fonts are approximated with the default CAD style. Raster content,
  nested forms, and mirrored or sheared text are skipped.
- **Recover DXF** uses ezdxf to repair an ASCII DXF and reports the audit fixes.
- **Combine DXF Contours** takes a DXF containing exactly two closed,
  straight-edged lightweight polylines. Difference subtracts the second
  modelspace entity from the first. The result is a separate DXF.


**File → Drawing Tools → Nest Parts on Sheet** places one closed straight-edged
lightweight polyline per part on a single rectangular sheet. Enter the sheet
dimensions and the part-to-part and edge clearance in drawing units. Enable
90° rotation unless material direction must be preserved. Packing uses part
bounding boxes, so it leaves clearance between the contours and does not
interlock concave profiles. Internal cutouts, curves, and other entity types
are outside this workflow. The result is a separate DXF with part outlines on
`NEST_PARTS` and the sheet reference on `NEST_SHEET`. Use the parts layer
for downstream cutting. If the placement fails, the tool leaves the source
and destination untouched.
