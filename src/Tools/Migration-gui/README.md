# conceald-migrate-gui

wxWidgets wizard for `conceald-migrate`. Collects old/new data directories and options, then runs the CLI migration tool in a subprocess and streams output into the window.

This folder is self-contained: sources, `CMakeLists.txt`, and `FindOrInstallWxWidgets.cmake` (wx dependency find/install).

## Build

```bash
cmake -DBUILD_MIGRATION_GUI=ON ..
make MigrationToolGui
```

When wxWidgets is not installed, CMake **tries** to install OS packages automatically (`MIGRATION_GUI_AUTO_INSTALL_DEPS=ON` by default):

| OS | Package(s) tried |
|----|------------------|
| Debian/Ubuntu | `libwxgtk3.2-gtk3-dev`, then `libwxgtk3.0-gtk3-dev` |
| Fedora/RHEL | `wxGTK3-devel` or `wxGTK-devel` |
| Arch | `wxwidgets-gtk3` |
| macOS | `brew install wxwidgets` |

Uses `sudo -n` first (non-interactive), then `sudo` (may prompt). Disable auto-install:

```bash
cmake -DBUILD_MIGRATION_GUI=ON -DMIGRATION_GUI_AUTO_INSTALL_DEPS=OFF ..
```

**Manual install**

- Debian/Ubuntu: `sudo apt-get install -y libwxgtk3.2-gtk3-dev`
- Fedora: `sudo dnf install -y wxGTK3-devel`
- Arch: `sudo pacman -S --needed wxwidgets-gtk3`
- macOS: `brew install wxwidgets`
- Windows: install [wxWidgets](https://www.wxwidgets.org/downloads/) and set `wxWidgets_ROOT_DIR` if needed

Build `MigrationTool` as well. The GUI looks for `conceald-migrate` in its own directory, then `../`, `../../`, etc. (CMake default: `build/src/Tools/conceald-migrate` next to `build/src/Tools/Migration-gui/conceald-migrate-gui`). Or set:

```bash
export CONCEALD_MIGRATE=/path/to/conceald-migrate
```

## Layout

- Left: old blockchain directory (`blocks.dat`, `blockindexes.dat`)
- Right: new MDBX **data directory** (parent of `mdbx_blocks`): path field with browse at the end; browse a parent then append a subfolder, or **Create folder** (`mkdir -p`). Warns if `mdbx_blocks` already exists (overwrite).
- Options: testnet, skip validation, batch size
- Output log and **Migrate** / **Cancel** (Stop while running)
