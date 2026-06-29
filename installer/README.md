# Varian Windows installer

A standard [Inno Setup](https://jrsoftware.org/isinfo.php) installer that gives
Windows users a one-click, world-class setup experience:

- installs `vn.exe`, the standard library (`vn_modules/`), headers, and the
  bundled runtime DLLs into `Program Files\Varian`
- adds Varian to the **system PATH** (so `vn` works in any terminal)
- sets `VARIAN_HOME`
- Start Menu shortcuts ("Varian Shell", documentation)
- a real **uninstaller** in *Settings → Apps* / Add-Remove Programs
- branded wizard (icon + welcome banner)

## Building locally

1. Build the Windows release and assemble the payload (the AppVeyor Windows job
   does this automatically into `dist\varian-windows-x64\`):

   ```sh
   mingw32-make release
   # then assemble dist\varian-windows-x64\ with vn.exe + vn_modules + DLLs + LICENSE
   ```

2. Compile the installer with the Inno Setup command-line compiler `ISCC`:

   ```sh
   iscc /DSourcePath=..\dist\varian-windows-x64 installer\varian.iss
   ```

   The result is `installer\output\varian-setup-0.1.0-x64.exe`.

On AppVeyor this runs automatically on a tagged build and the resulting
`varian-setup-*.exe` is attached to the GitHub Release.

## Assets

- `assets/varian.ico` — multi-resolution app icon
- `assets/wizard-large.bmp` / `assets/wizard-small.bmp` — wizard branding

To re-brand (e.g. swap in a Kiln or Constellation banner), replace the BMPs
(164×314 and 55×58, 24-bit) and rebuild.
