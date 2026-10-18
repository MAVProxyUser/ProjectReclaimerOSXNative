#!/usr/bin/env python3
"""Invisible walls of a Halo 3 (MCC) map: the collision surfaces flagged invisible (structure BSP + instanced geometry),
for the zones overlay (shim/src/hitbox.c hbz_load, backlog 18).

    python3 re-tools/h3inv.py <file.map> <out.inv.bin>

Output: b"RIW1", u32 triangle count, then per triangle 9 float32 (three world-space vertices). Read-only file work
(re-tools/h3bsp.py decodes the collision BSPs). Valhalla has ~1,800 such triangles, Guardian ~1,400, The Pit ~200.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import h3bsp  # noqa: E402
from h3map import H3Map  # noqa: E402


class Opts:
    invisible = True
    unmaterialed = True
    no_instances = False
    render_only = False


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    m = H3Map(sys.argv[1])
    res = h3bsp.Resources(m)
    tris, _kinds, _two, flags = h3bsp.level_triangles(m, res, Opts, log=lambda *a: None, with_flags=True)
    t = np.asarray(tris, np.float32)[(np.asarray(flags) & h3bsp.SF_INVISIBLE) != 0].reshape(-1, 9)
    tmp = sys.argv[2] + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'RIW1' + struct.pack('<I', len(t)) + t.tobytes())
    os.replace(tmp, sys.argv[2])
    print('%s: %d invisible-wall triangles -> %s' % (os.path.basename(sys.argv[1]), len(t), sys.argv[2]))


if __name__ == '__main__':
    main()
