# viva-gpu on the XCover5

The Metal driver built here is **viva-gpu** by Nik300 — https://github.com/Nik300/viva-gpu — released under the
GNU General Public License v3.0 or later, with an additional term (section 7(b)): redistributions and derivative
works must keep the author attribution and the credits in viva-gpu's `NOTICE` and show them in their documentation.
`build-viva.sh` fetches it at a fixed commit and copies its `NOTICE` and `LICENSE` next to the bundle; the drop zone
installs them with it.

Files in this directory:
- `vgpu_xc5.c` — a vgpu back-end for the XCover5 kernel's XC5GPU user client (ios-xcover5). As a part of the
  viva-gpu build it is distributed under the same licence (GPL-3.0-or-later).
- `build-viva.sh` — fetches viva-gpu, adds the back-end, builds `VivaMTLDriver.bundle` (Mesa Panfrost, AIR -> NIR).

Credits (from viva-gpu): Mesa and the Panfrost developers at Collabora and in the community; Arm, for the Valhall
ISA description shipped in Mesa; ios-sweet, the XNU port ios-viva builds on.
