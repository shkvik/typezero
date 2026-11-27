# LLVM-C for self

- `win32-x64/LLVM-C.lib` — the import library of `LLVM-C.dll` from LLVM 23.1.4 for Windows x64
  (`lib/LLVM-C.lib` of the LLVM install). `self/package.json` links it through `native`.
- The DLL itself (74 MB) is not kept here: `LLVM-C.dll` of the same LLVM lies next to
  `tzparse.exe` (`bin/LLVM-C.dll` of the LLVM install).
- `LICENSE.TXT` — `LICENSE.TXT` of llvm-project.
