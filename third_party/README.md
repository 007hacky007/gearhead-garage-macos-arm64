# Reused source

The x86-to-C translator (`tools/lift.py`, `tools/lregs.py`, `tools/genall.py`) and
the Win32 compatibility runtime under `runtime/` were adapted from the sibling
NFS IV for Apple Silicon project, which in turn adapted them from NFSU2Recomp
and nfsu2-web. Their licenses are kept here. The NFSU2Recomp-derived portions
permit personal local use and modification; redistribution requires permission
under their license.

New for Gearhead Garage: DirectDraw (`runtime/ddraw.c`), Direct3D 5 on Metal
(`runtime/d3d.c`), DirectSound 3D (`runtime/sound3d.c`), the WinMM file I/O and
other calls in `runtime/gearhead_api.c`, the disc, MSI and cabinet readers
(`tools/disc.py`, `tools/msi.py`) and the launcher.

No game code or assets belong in this source tree. Game-derived output is
written only under the ignored `local/`, `gen/` and `out/` folders.
