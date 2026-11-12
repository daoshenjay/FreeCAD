# SWDWG

A native DWG import module for FreeCAD, built on **LibreDWG + OpenCASCADE**, with a **pure-C++ import pipeline** (no Python geometry wrapping).

## Features

- **Pure-C++ import pipeline**: `dwg_read_file → parse → entity extraction → OCC geometry building → layer aggregation → object creation`. Better performance and control at ten-thousand-entity scale.
- **Mojibake elimination**: multi-candidate decoding (GBK / UTF-8 / Latin-1) with a scoring pick for TEXT / MTEXT throughout.
- **Faithful geometry**: bulge arcs, circle→ellipse, 3-point ARC, SPLINE (quadratic B-spline), degenerate-geometry fallbacks.
- **HATCH restore**: `.pat` infinite line families → boundary intersection clipping → dash phase.
- **Text & dimensions**: TEXT/MTEXT → `SWDWG::DWGText`; DIMENSION → `SWDWG::DWGDimension`. Both SHX (geometric) and TTF (`SoAsciiText`) glyph rendering with shx→TrueType font mapping.
- **Layer aggregation**: entities sharing a layer style (color / lineweight / linetype) are fused into a single `Part::Feature`, dropping the object count from "entities" to "style groups".
- **Complex linetypes**: 3-level mapping L0 name → L1 16-bit quantized pattern → L2 geometric expansion, including SHX shape / TTF text elements inside complex linetypes.
- **Performance work**: zero-copy block geometry reuse, import/restore bulk suppression, render caching with exact invalidation, pure-C++ import, status-bar progress UI, etc.
- **SHX viewer**: the `SWDWG_ViewSHX` ("View SHX Font…") command draws every shape/glyph of a `.shx` file on a grid, handy to verify shape decoding.

## Architecture (overview)

```
DWG binary
  → Parse      App/DWGImporter.cpp::parse()   (LAYER/STYLE/DIMSTYLE/LINETYPE/BLOCK tables)
  → Extract    App/DWGExtract.cpp             (Dwg_Object → Entity, zero FreeCAD dependency)
  → Build      App/DWGShape.cpp / DWGHatch    (Entity → OCC TopoDS_Shape)
  → Expand     App/DWGImporter.cpp::expand()  (block expansion / transforms / style grouping / object creation)
  → FreeCAD    App::Document:
               DWGLineGroup(Part::Feature) · SWDWG::DWGText · SWDWG::DWGDimension
  → GUI        Gui::Document:
               ViewProviderPart · ViewProviderDWGText · ViewProviderDWGDimension
```

Dependency rule: **the App layer never depends on the Gui layer**; the Gui layer only reads App objects through the property system and never touches LibreDWG data directly.

| DLL | Contents | Dependencies |
|---|---|---|
| `SWDWG` (App, Python module) | import pipeline, geometry, text/dimension objects, preview reader | LibreDWG · OpenCASCADE · FreeCAD App/Base · PyCXX |
| `SWDWGGui` (Gui, Python module) | import command & style mapping, text/dimension view providers | SWDWG App · Coin3D · FreeCAD Gui |

## Building

Prerequisites: a FreeCAD source tree (App/Gui/Base), LibreDWG, OpenCASCADE, Coin3D (Qt).

1. Place the module in the FreeCAD source tree at `src/Mod/SWDWG` and build it together with the top-level FreeCAD CMake (the module ships its own `CMakeLists.txt` producing two Python-extension targets: `SWDWG` and `SWDWGGui`).
2. LibreDWG is integrated via `find_package` or as a subdirectory (on MSVC pass the ABI macros required by the module `CMakeLists.txt`).
3. `SWDWGGui` loads only in GUI sessions; pure console use (`FreeCADCmd`) can still import DWG through `SWDWG`.

> Unit tests / tools: `tests/` ships standalone unit tests and glyph probe tools that compile and run without FreeCAD/OCC.

## Usage

SWDWG is a **file-format (library) module**, not a workbench: it adds no entry to the workbench selector, and DWG is reached through the standard File menu in any workbench.

### GUI (FreeCAD graphical session)

1. Menu **File → Import…**, choose the `Autodesk DWG (*.dwg *.DWG)` filter, then pick the `.dwg` file
   (**File → Open** works too — the open dialog lists the same DWG filter; SWDWG is the only
   module claiming `dwg`, so no module-selection dialog appears);
   - If no document is open a new document named after the DWG file is created automatically;
   - Import progress is shown as a **status-bar progress bar** at the bottom of the main window (short tasks never flash any window); when finished the status bar shows elapsed time and object count;
   - Geometry is grouped per layer style; color / lineweight / linetype are mapped back onto the view;
   - Large drawings are not made undoable step-by-step (to avoid memory blow-up); revert by deleting objects/documents.
2. Export: **File → Export…** with format `Autodesk DWG (*.dwg)` (registered by `SWDWG/Init.py` and served by `export_dwg.py`, which writes DWG directly, no DXF step).
3. The diagnostic / developer commands have no menu entry; run them from the Python console:
   ```python
   Gui.runCommand("SWDWG_Import")         # new document + import a DWG
   Gui.runCommand("SWDWG_Export")         # export current / selected objects
   Gui.runCommand("SWDWG_ViewSHX")        # draw every shape/glyph of a .shx on a grid
   Gui.runCommand("SWDWG_MultiInstance")  # SoMultipleInstance demo
   ```

### Command line

```python
# FreeCADCmd
import SWDWG
SWDWG.readDWG("a.dwg")               # preview read (returns a dict)
SWDWG.open("a.dwg")                  # create a uniquely-named document and import
SWDWG.insert("a.dwg", "docName")     # import into the given / active document
```

## Repository layout

```
SWDWG/
├── App/            # SWDWG DLL: pipeline, geometry, text/dimension objects, preview reader
├── Gui/            # SWDWGGui DLL: commands/view providers, import & open progress
├── tests/          # standalone unit tests and glyph probe tools
├── Init.py         # App module Python entry (registers the .dwg import/export)
├── InitGui.py      # GUI module entry (registers commands and view providers, no workbench)
├── CMakeLists.txt  # module build
└── LICENSE         # license
```

## License

See [LICENSE](LICENSE).
