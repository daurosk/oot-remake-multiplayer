## In the NX2navi mod (`OoT Co-op` folder)

| Component | Used for | Licence | Source |
|---|---|---|---|
| `plugin_api.h`, NX2navi plugin interface version 1 (Copyright (c) 2026 chrg6) | `bin\coop_nx2navi.dll`: the declarations of the game's plugin interface (in-game overlay, keys, chat box). Copied unchanged into `nx2navi/dll/third_party/plugin_api.h`. | The permission in the file's own header: copying it into a mod for NX2navi, compiling it in and distributing it with the mod under the mod's licence, provided the notice stays in every copy; provided as is, without warranty. It covers that file alone. | NX2navi `runtime/plugin_api.h` |
| fmt 12.1.0 (Victor Zverovich and contributors) | `bin\coop_nx2navi.dll`: the log lines of the co-op hook's own files (`hookshot/compat/common/logging.h`), built in header-only | MIT; the licence text is appended to this file in the mod folder by `nx2navi/build.py` | https://github.com/fmtlib/fmt, fetched by `nx2navi/build.py` with git at the commit `hookshot/CMakeLists.txt` pins; the build stops unless git reports that commit |
