# .osa — atmosphere (kind `ATMO`, version 4)

Layout: [`src/formats/osa.hpp`](../../src/formats/osa.hpp). One file per import, `atmosphere/weather.osa`: what the sky, the weather and exterior lighting are
computed from. How the engine uses it.

| Chunk | Contents |
|---|---|
| `SETT` | `SkySettings`: the sky's game settings (GMST) and the TimeScale global after the load order, engine defaults where no plugin sets one |
| `WTHR` | `Weather[]`: FormID, EditorID, the 17 colour types x 4 times (NAM0, rgba8), fog distances/powers/maxima (FNAM, units), directional ambient per time (DALC), image spaces (IMSP) and volumetric lighting (HNAM) per time as FormIDs, DATA bytes, precipitation (MNAM), sky model (MODL), cloud layer count (LNAM), disabled layers (NAM1), thunder entries, sky statics range, 32 cloud layers |
| `CLMT` | `Climate[]`: FormID, EditorID, timing (TNAM), range of the climate's weather list, sun and glare textures, stars model |
| `WLST` | `ClimateWeather[]`: weather FormID and chance, per climate; a chance global's value replaces the chance |
| `WRLD` | world EditorID (lowercase) -> climate FormID, following parents whose PNAM says "use climate" |
| `IMGS` | `ImageSpace[]`: HNAM, CNAM, TNAM, DNAM as floats |
| `VOLI` | `Volumetric[]`: the twelve floats |
| `SPGD` | `Precipitation[]`: DATA and the texture; forms with the 40-byte DATA are left out |
| `IMAD` | `Modifier[]`: DNAM's fields and the curves, as ranges of `KEYS` |
| `KEYS` | floats: time, value pairs, or time, r, g, b, a for colour curves |
| `STAT` | the weathers' TNAM STAT FormIDs |
| `REGN` | regions with weather lists: `Region` (FormID, RDAT override and priority, a span of `WLST`, a span of `RARA`) |
| `RARA` | their areas: `RegionArea`, a span of `RPTS` |
| `RPTS` | area polygons, x y float pairs in Skyrim units |
| `RCEL` | exterior cells listing those regions: `RegionCell` (index in `WRLD`, grid x y, a span of `RLST`), sorted by world, x, y |
| `RLST` | indices in `REGN`, each cell's XCLR order |
| `SKYM`, `SKYS`, `SKYV`, `SKYI` | the sky's meshes: models, shapes (type, cloud layer, texture, index range), vertices (node transforms baked, game units), indices |
| `WATR` | `Water[]`: every water form, sorted by FormID: noise textures (NNAM), related waters (GNAM), image space (INAM), FNAM, ANAM, NAM0 and DNAM as floats |
| `STRS` | string table: EditorIDs, texture and model paths |

## Semantics

- Times of day are indexed 0 sunrise, 1 day, 2 sunset, 3 night, as in the records.
- Load-time fix-ups of the game are applied: a colour table of 0xC0 bytes or less copies Fog Near into Fog Far, an all-zero Effect Lighting takes Sunlight, a layer without a texture is set in the
  disabled mask. A missing IMSP entry uses `DefaultImageSpace` (0x167). A DALC shorter than 32 bytes keeps specular 0 and fresnel 1; an FNAM of 24 bytes keeps the maxima at 1.
- Cloud layer i of a weather pairs with the shape of `meshes/sky/clouds.nif` whose layer is i: the file order of its cloud shader properties.
- Values are the records' own (units, bytes); nothing is converted. A DNAM's fourth value stays a float: the engine truncates it, like the game.
- Weather sky models are ordinary meshes (`.osm`): the path here ends in `.nif` and names the source.
