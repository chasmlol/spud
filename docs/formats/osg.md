# .osg — game world (kind `GWLD`, version 1)

Layout: [`src/formats/osg.hpp`](../../src/formats/osg.hpp) (records with `static_assert`ed sizes are the source of truth). Written by the importer (`skytool import`, or `skytool ui world-tables` for this file alone) to `<cache>/rules/world.osg`; read by the world simulation. About 2 MB, rewritten on every import. Positions are Skyrim units, angles radians.

| Chunk | Contents |
|---|---|
| `MARK` | map markers: every MapMarker static with XMRK, its worldspace, cell, name, position, z angle, XRDS radius, FNAM flags, TNAM type, record flags, enable parent, and the point fast travel arrives at (the no-keyword XLKR, else the marker) |
| `WRLD` | worldspaces: parent and parent flags, DATA flags, MNAM map data (usable size, corner cells, camera heights and pitch), ONAM map offset, bounds, encounter zone and location, and the space holding it (a merged city: its parent) |
| `ZONE` | encounter zones (ECZN DATA): owner, location, rank, levels, flags |
| `LOCN` | locations: name, parent, world location marker and radius, keywords |
| `CELL` | cells with an encounter zone, a location or non-zero DATA flags: worldspace, grid, flags, zone, location |
| `KYWD` | `u32` keyword FormIDs, indexed by `LOCN` |
| `DOOR` | load doors (REFR with XTEL): worldspace, cell, destination door, position, exit position, flags (minimal use, initially disabled) |
| `LREF` | location special references: location, reference type (LCRT), reference |
| `WNAM`, `CNAM`, `CEDI` | parallel to `WRLD` and `CELL`: each one's name (FULL), a cell's EditorID |
| `REGN`, `RPTS` | each area of a region with a map name (RDMP): worldspace, name, its x, y points |
| `STRS` | string table |
