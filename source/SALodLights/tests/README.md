# Procedural limits checks

The host-side test exercises the production list/patch helpers: exhausting and
rebuilding extended pools, repeated terrain reloads, failed-allocation recovery,
and decoding 16/32-bit comparison immediates without matching unrelated data.

From the repository root, with an existing temporary output directory:

```sh
clang -std=c11 -Iexternal/injector/zydis -c external/injector/zydis/Zydis.c -o "$TMPDIR/procedural-zydis.o"
clang++ -std=c++20 -I. -Iexternal/injector/zydis source/SALodLights/tests/ProceduralLimitsTests.cpp "$TMPDIR/procedural-zydis.o" -o "$TMPDIR/procedural-tests"
"$TMPDIR/procedural-tests"
```

The SALodLights Windows build separately checks the 32-bit game structure sizes
and offsets at compile time. Host tests cannot execute the actual game hooks.

## In-game verification

- Check `SALodLights.procedural.log` beside the INI for the installed capacities.
  Incompatible comparison instructions or failed hook creation are logged and the
  procedural-limit feature is left disabled rather than partially installed.
- Keep draw distance 40 and the original 80/100 generation distances. Compare the
  same vegetation-heavy route with vanilla and increased capacities.
- Check grass as well as procedural bushes, including sloped terrain (matrices).
- Load another save/start a new game repeatedly, and enter/leave interiors.
- On procedural-manager shutdown the log records peak tracking usage, failed
  creations, and recovered entries. A failure count alone does not identify which
  engine check rejected creation.
- Test with OLA's enlarged `Buildings`/`Objects` pools and `MatrixList = unlimited`.
  These remain separate from SALodLights' procedural limits.

All five capacity settings are startup-only. Values below vanilla are raised to
vanilla; values above 65535 are capped. Setting all five to vanilla disables the
new hooks. Extending capacities does not change placement spacing or distances.
