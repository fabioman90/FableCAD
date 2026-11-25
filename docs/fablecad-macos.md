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
