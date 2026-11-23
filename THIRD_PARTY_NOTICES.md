# Third-party notices

## Application UI icons

The SVG files in `assets/icons/ui/` and `assets/icons/ui-dark/` are
project-authored geometric artwork, redrawn on a 20 × 20 grid. They are
covered by the project's [MIT license](LICENSE). The application does not embed Lucide/Feather UI assets.

## Other runtime dependencies

Qt, Exiv2, LibRaw, libjpeg-turbo, and any enabled LittleCMS or transitive runtime dependencies
retain their respective licenses. This document is not a complete runtime
license inventory. Packaged builds additionally contain
`RUNTIME_DEPENDENCIES.json`, `RUNTIME_DEPENDENCIES.md`, and the collected
evidence under `licenses/runtime/`. This records the actual deployed files
and available notices, not a complete compatibility or source-delivery approval.
Distributors must supply the notices and source
materials required by the actual libraries included in their packages.

Exiv2 is GPL-2.0-or-later; bundling it requires a compatible distribution
of the combined application, independently of the project's own license.

## Diagnostic archives and crash capture

- miniz 3.0.2 is statically linked for ZIP export. Source: https://github.com/richgel999/miniz/tree/3.0.2 . Full upstream license: `third_party/miniz/LICENSE`, distributed as `licenses/miniz-LICENSE`.
- macOS embeds Crashpad revision `4b8fc2b536e04407032cb6eba687cfe68cb5966a` and its pinned mini_chromium dependency. Source: https://chromium.googlesource.com/crashpad/crashpad/ . These components are built by `scripts/build_crashpad_macos.sh`; their upstream licenses are copied into the application resource licenses directory at packaging time. Crash reporting is local only; network uploads are disabled.
- `mvpview_crash_handler.exe` is this project's Windows helper using the Windows DbgHelp API.
