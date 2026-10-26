# NBA Live 06 — Bench configuration in CustomPaths.cpp

Updated file: `CustomPaths_Bench.cpp` (replace your existing `CustomPaths.cpp` after reviewing local changes).

Per-stadium file: `assets\stadia\<four-character stadium id>\bench.json`. Example: `assets\stadia\bost\bench.json`.

The JSON has top-level `home` and `away` objects; each has a `slots` array containing exactly seven objects, numbered 1–7 in array order. `pose` can be `default`, `seated`, or `floor`. Missing file or omitted pose defaults to original EA selection. A JSON parse failure leaves every slot at its native behavior. The sample config forces slots 5 and 7 to seated for both sides; change those to `default` for vanilla seating.

**Important:** `positionOffset: [x,y,z]` and `yawOffset` are parsed but deliberately NOT applied. These need a verified final actor-transform writer; blindly writing animation or game-world transforms can break reaction motions and substitutions. The DLL prints a diagnostic if they are requested. Other versions (05/07/08) retain existing behavior.

**Build/runtime constraints:** The hooks only install for the existing NBA Live 06 executable check in `Initialize()`. Both sites are byte-validated against the ORIGINAL unpatched EXE. Run `nbalive06.exe` without the separate experimental bench animation patch; the experimental EXE would defeat the `default` behavior. No additional JSON library is required. C++17/MSVC x86 is assumed. Existing camera/stadium override hooks remain untouched.

**Hook sites:** `0x005ED0C2` before initial bench actor animation dispatch and `0x005ED458` before its state transition/restart. Both replace 7 complete verified bytes with 5-byte jumps and NOPs, execute the displaced instructions, then rejoin native code. For each call the slot index and actor address are cross-checked against the appropriate home/away 0x1100-stride array. Missing/invalid data returns the native parameter. The `floor` selector uses animation parameter 7 inferred from original special slots; its behavior on normally seated slots is experimental and needs runtime testing.

**Testing:** First test no bench JSON on an original EXE (vanilla). Next copy example and confirm 5/7 seated as in the prior EXE experiment. Test `floor` on one normally seated slot and a substitution. Do not attempt position calibration yet.
