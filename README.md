# TreeFern

[![Build](https://github.com/DocRountree/treefern/actions/workflows/build.yml/badge.svg)](https://github.com/DocRountree/treefern/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/DocRountree/treefern)](https://github.com/DocRountree/treefern/releases/latest)

TreeFern is a lightweight, free and open-source editor for cladograms, written in C++ with Qt 6.

Cladograms are stored as plain JSON files. Every edit is saved immediately, and changes made to the file by other programs are picked up live.

<img width="1705" height="1414" alt="TreeFern showing the standard layout" src="https://github.com/user-attachments/assets/124aef28-8f36-4c05-a570-cf27de30c690" />

## Contents

- [Features](#features)
- [Download](#download)
- [Building from source](#building-from-source)
- [Usage](#usage)
- [Keyboard and mouse reference](#keyboard-and-mouse-reference)
- [File format](#file-format)
- [Screenshots](#screenshots)
- [Contributing](#contributing)
- [License](#license)

## Features

- **Two layouts**
  - **Standard**: a tapered branching tree in which heavier daughter clades curve away from their mother clade, inspired by the [OneZoom](https://www.onezoom.org/) viewer. Spread, shrink and curl can be adjusted from the keyboard.
  - **Disks**: nested circle packing, with each daughter clade drawn as a disk inside its mother clade.
- **Editing**: add daughter clades, rename clades and attach free-form notes to them, collapse or expand subtrees, delete subtrees, and move a subtree to a new mother clade with cut and paste.
- **Undo and redo** for every edit.
- **Autosave and live sync**: each change is written to disk atomically, and the file is reloaded automatically if another program modifies it.
- **Dynamic labels**: labels too small to read are hidden until you zoom in, and labels reposition themselves to avoid overlapping. Labels can be placed over or beside nodes.
- **Deep zoom**: the camera re-anchors to nearby clades while zooming, so very large and very deep trees stay precise at any zoom level.
- **Search and navigation**: find clades by name from the side panel, and move around the tree entirely by keyboard or entirely by mouse.
- **Export**: save the current view as a PNG image (at twice screen resolution) or as a scalable SVG.
- **Dark and light themes.**
- **Remembered preferences**: layout, label placement, theme and recently opened files persist between sessions.

## Download

Prebuilt packages are attached to each [release](https://github.com/DocRountree/treefern/releases/latest).

| Platform | File | Notes |
|---|---|---|
| Windows 10/11 (64-bit) | [`TreeFern-Setup.exe`](https://github.com/DocRountree/treefern/releases/latest/download/TreeFern-Setup.exe) | Installer. Installs for the current user and does not require administrator rights. |
| Windows 10/11 (64-bit) | [`treefern-windows.zip`](https://github.com/DocRountree/treefern/releases/latest/download/treefern-windows.zip) | Portable version. Unzip and run `treefern.exe`. |
| Linux (x86-64) | [`TreeFern-x86_64.AppImage`](https://github.com/DocRountree/treefern/releases/latest/download/TreeFern-x86_64.AppImage) | Single self-contained file. Requires glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or later). |
| All | [`TreeFern-samples.zip`](https://github.com/DocRountree/treefern/releases/latest/download/TreeFern-samples.zip) | Sample cladograms. The Windows packages already include them next to `treefern.exe`. |

**Windows:** the installer is not code-signed, so Windows SmartScreen may show "Windows protected your PC". Click **More info**, then **Run anyway**.

**Linux:** make the AppImage executable, then run it:

```sh
chmod +x TreeFern-x86_64.AppImage
./TreeFern-x86_64.AppImage
```

macOS packages are not yet available; macOS users can build from source.

## Building from source

### Requirements

- A C++17 compiler (GCC, Clang or MSVC)
- CMake 3.16 or newer
- Qt 6 with the Widgets and Svg modules

Installing the dependencies:

| System | Command |
|---|---|
| Debian, Ubuntu, Mint | `sudo apt install build-essential cmake qt6-base-dev qt6-svg-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtsvg-devel` |
| Arch, Manjaro | `sudo pacman -S base-devel cmake qt6-base qt6-svg` |
| openSUSE | `sudo zypper install gcc-c++ cmake qt6-base-devel qt6-svg-devel` |
| macOS (Homebrew) | `brew install cmake qt` |
| Windows | Install Qt 6 with the [Qt Online Installer](https://www.qt.io/download-qt-installer) and CMake, or use [vcpkg](https://vcpkg.io/). |

### Build

```sh
git clone https://github.com/DocRountree/treefern.git
cd treefern
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The program is a single source file, so downloading just `main.cpp` and `CMakeLists.txt` into one directory and running the last two commands also works.

The executable is written to `build/treefern` (Linux), `build/Release/treefern.exe` (Windows with MSVC) or `build/treefern.app` (macOS).

## Usage

Start TreeFern from your application menu, by double-clicking the executable, or from a terminal:

```sh
./build/treefern                 # reopen the last file, or show the start prompt
./build/treefern cladogram.json  # open a file, creating it from the sample tree if it does not exist
```

On first launch TreeFern offers to open an existing cladogram, create an empty one, or create one from a built-in sample tree. Files can also be opened from **File > Open**, **File > Open Recent**, or by dragging a JSON file onto the window.

Typical workflow:

1. Click a clade to select it. Its name, position in the tree and notes appear in the side panel.
2. Press **A** to add a daughter clade, or **E** to edit the clade's name and notes.
3. To move a subtree, select it and press **X**, then select the new mother clade and press **V**.
4. Press **Ctrl+Z** to undo any mistake.

There is no save command: every change is written to the file as soon as it is made. Use **File > Save As** to continue editing under a new file name.

Sample cladograms are in the [`samples`](samples) directory.

## Keyboard and mouse reference

### Mouse

| Action | Effect |
|---|---|
| Click | Select a clade |
| Double-click | Edit a clade |
| Right-click | Clade context menu |
| Drag | Pan |
| Scroll wheel | Zoom (hold Shift to zoom faster) |

### Editing

| Key | Action |
|---|---|
| A | Add a daughter clade to the selected clade |
| E or R | Edit the selected clade's name and notes |
| C | Collapse or expand the selected clade |
| Delete or Backspace | Delete the selected clade and its descendants |
| X or Ctrl+X | Cut the selected subtree |
| V or Ctrl+V | Paste the cut subtree under the selected clade |
| Esc | Cancel a pending cut, or clear the selection |
| Ctrl+Z | Undo |
| Ctrl+Shift+Z (also Ctrl+Y on Windows) | Redo |

### Navigation and view

| Key | Action |
|---|---|
| Up | Go to the heaviest daughter clade |
| Down | Go to the mother clade |
| Left / Right | Go to the neighbouring sister clade |
| F | Focus the view on the selected clade |
| Home | Fit the whole tree in the view |
| Ctrl+F | Search clades by name |
| Ctrl+1 / Ctrl+2 | Standard / Disks layout |
| L | Toggle labels over or beside nodes |
| Ctrl+Shift+D | Toggle dark mode |
| + or = / - | Increase / decrease branch spread (Standard layout) |
| [ / ] | Decrease / increase branch shrink (Standard layout) |
| , / . | Curl branches counter-clockwise / clockwise (Standard layout) |

### Files

| Key | Action |
|---|---|
| Ctrl+N | New cladogram |
| Ctrl+O | Open |
| Ctrl+Shift+S | Save As |
| Ctrl+E | Export the current view as PNG or SVG |
| Ctrl+Q (Linux and macOS) | Quit |

On macOS, Ctrl corresponds to the Command key.

## File format

A cladogram is a JSON object with the id of the root clade and a flat list of clades. Every clade except the root names its mother clade in `parent`.

```json
{
  "root": "a1",
  "nodes": [
    { "id": "a1", "name": "Apes", "info": "Superfamily Hominoidea", "collapsed": false },
    { "id": "b2", "parent": "a1", "name": "Great Apes", "info": "", "collapsed": false },
    { "id": "c3", "parent": "a1", "name": "Gibbons", "info": "", "collapsed": true }
  ]
}
```

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique identifier for the clade |
| `parent` | string | Id of the mother clade; omitted for the root |
| `name` | string | Display name |
| `info` | string | Free-form notes, shown in the side panel |
| `collapsed` | boolean | Whether the clade's descendants are hidden |

Because the format is plain JSON, files can be generated or edited by scripts and other tools while TreeFern is open; it reloads them automatically. Clades whose parent chain does not reach the root are ignored when a file is loaded.

## Screenshots

**Standard and Disks layouts**

<img width="1705" height="1414" alt="Standard layout" src="https://github.com/user-attachments/assets/124aef28-8f36-4c05-a570-cf27de30c690" />
<img width="1717" height="1420" alt="Disks layout" src="https://github.com/user-attachments/assets/787e1073-e624-46f7-ae94-a4d7b69feb1d" />

**Light mode**

<img width="1720" height="1421" alt="Light mode" src="https://github.com/user-attachments/assets/7cbe0f3a-47f7-4e0a-9221-42b328ce9440" />

**Labels beside nodes**

<img width="1718" height="1421" alt="Labels beside nodes" src="https://github.com/user-attachments/assets/e501e873-f8ca-4104-8ab2-2d8eace6a696" />

More screenshots are in the [screenshot gallery](screenshot-gallery.md).

## Contributing

Bug reports, feature requests and pull requests are welcome on the [issue tracker](https://github.com/DocRountree/treefern/issues).

- The whole application is in [`main.cpp`](main.cpp). The comment at the top of the file describes its architecture.
- Every pull request is built on Linux, Windows and macOS by [GitHub Actions](.github/workflows/build.yml).
- Releases are made by publishing a GitHub release with a `v*` tag (for example `v1.1.0`); the build then attaches the Windows and Linux packages automatically.

## License

TreeFern is free software, distributed under the terms of the GNU General Public License. See [LICENSE](LICENSE) for the full text.
