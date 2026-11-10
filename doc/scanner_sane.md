# Scanning with SANE (Linux / BSD)

OpenToonz can scan through [SANE](http://www.sane-project.org/) when it is
built with `libsane-dev` (see `how_to_build_linux.md`). The driver is
`TScannerSane` in `toonz/sources/tnzbase/tscanner/`. It is optional: without
the library the build is unchanged and TWAIN/Epson behave as before.

## Using it

1. **Scan > Define Scanner**, choose the **SANE** driver (the default on
   Linux when built in), then pick a device under **SANE Device** (press
   **Refresh** after plugging one in; network scanners can take a few seconds).
2. Set paper size, dpi, mode (Black&White / Gray / Color), brightness and
   threshold in the scan settings, then **Scan**.
3. A scanner with an automatic document feeder is used as such when
   **Paper Feeder** is enabled; Cancel stops between chunks of data.

Supported image formats: 1-bit lineart, 8-bit gray, 8-bit RGB; 16-bit is
down-converted to 8-bit. Separate-plane (three-pass) colour is rejected with
an error message.

## Permissions

If scanning fails with an access error, your user needs rights on the USB
device: join the `scanner` group (`sudo usermod -aG scanner $USER`, then log
in again) and check that `sane-utils`/`libsane` ships its udev rules.
Verify outside OpenToonz with `scanimage -L`. Network scanners need `net`
(saned) or `airscan` (eSCL) enabled in `/etc/sane.d/dll.conf`.

## Testing without hardware

SANE's `test` backend simulates a scanner with every format OpenToonz must
handle. Run the automated matrix (uses a private config dir, no root needed):

    toonz/sources/tnzbase/tscanner/tests/sane_matrix.sh

It checks gray/colour/lineart at 1/8/16 bits, four test pictures, scan-window
geometry, hand-scanner (unknown height), three-pass, and an ADF that runs dry
after 10 pages. It validates the backend side; the OpenToonz side is the
manual checklist below.

To use the test backend in OpenToonz itself, uncomment `test` (and `pnm`) in
`/etc/sane.d/dll.conf`, or start OpenToonz with a private config:

    mkdir /tmp/sanecfg && printf 'test\n' > /tmp/sanecfg/dll.conf
    cp /etc/sane.d/test.conf /tmp/sanecfg/
    SANE_CONFIG_DIR=/tmp/sanecfg ./OpenToonz

### Manual checklist (OpenToonz, test backend)

| # | Setup | Expect |
|---|-------|--------|
| 1 | Device `test:0`, Color, 100 dpi, "Color pattern" | Level created, colours not swapped (compare with `scanimage --test-picture "Color pattern" --mode Color`) |
| 2 | Gray, 8-bit | Gray level, same orientation as scanimage output |
| 3 | Black&White (lineart) | 1-bit result, **not inverted** (white page stays white), not flipped |
| 4 | Brightness / threshold changes | Visible change in result |
| 5 | Paper size A4 vs crop box | Raster size matches the selected area at the chosen dpi |
| 6 | Paper Feeder on, 3 pages | 3 frames added; feeder "out of documents" ends the run without an error |
| 7 | Paper Feeder on, 12 pages | Stops cleanly after 10 pages with a message, earlier frames kept |
| 8 | Cancel during a large scan (1200 dpi) | Returns promptly, no crash, scanner usable again |
| 9 | Hand scanner option (set via `test.conf` / scanimage not available in UI) | n/a in UI; covered by sane_matrix.sh |
| 10 | Three-pass colour | Clear error message, no crash |
| 11 | Unplug / no device | "No SANE scanner is available" |
| 12 | Switch driver SANE -> Internal -> SANE | Settings and device choice preserved |
| 13 | Build without libsane | CMake says `disabled`; no SANE entry in Define Scanner; TWAIN/Epson unchanged on Windows |

Hardware pass (before release): one USB flatbed, one with ADF, one network
(airscan or net) device; compare output with `scanimage` at the same settings.

## Packaging notes

* Build dependency: `libsane-dev` (Debian/Ubuntu), `sane-backends-devel`
  (Fedora/openSUSE), `sane` (Arch).
* Runtime dependency: `libsane1` plus backends (`sane-utils`; `sane-airscan`
  for driverless network scanners). libsane is a dynamic dependency; the
  frontend licence exception in SANE permits linking.
* AppImage/Flatpak: SANE backends must come from the host (or the Flatpak
  needs access to USB and the SANE socket); do not bundle libsane without its
  backends.
* Translations: new strings use `tr()` in `scanpopup.cpp`; they are picked up
  by the normal `lupdate` run at release time.
