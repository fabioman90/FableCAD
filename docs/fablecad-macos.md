# FableCAD on macOS

## Interface and packaging

The project uses the FableCAD name, logo, and icons. This update covers the toolbars, commands, work panels, Italian localization, and macOS bundle configuration. The ARM64 DMG for version 2.2.9 was signed, notarized by Apple, and tested on the development Mac.

## Opening and saving DWG files

The experimental bridge uses acadrust 0.6.8. Build it with `libraries/acadrust_bridge/build-experimental.sh` and set `FABLECAD_ACADRUST_HELPER` to the executable path.

The bridge retains the source DWG document and verifies the temporary output before replacing the destination. Tests covered 2D geometry, text, dimensions, layers, simple blocks, and solid and patterned hatches. The samples retained dynamic block definitions and the opaque DATATABLE data in a DWG 2004 file.

AutoCAD 2020 uses DWG 2018. Sample drawings were checked with LibreDWG and ODA. Direct testing in AutoCAD 2020 is pending.

## Updates

The release checker can notify users of newer GitHub releases at startup and from the menu. On macOS, Sparkle 2 can install a differential update when the feed provides a patch for the installed version.

Set the optional CMake variables `FABLECAD_SPARKLE_FRAMEWORK`, `FABLECAD_SPARKLE_FEED_URL`, and `FABLECAD_SPARKLE_PUBLIC_KEY` to the framework path, HTTPS appcast URL, and public EdDSA key.

Local testing verified the 2.2.9 → 2.2.10 patch, confirmed that the patched bundle matched the target, and rejected a modified patch. The production HTTPS feed and the full install-and-relaunch cycle from a distribution-signed app remain to be tested.
