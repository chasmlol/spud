# SPUD container

Every binary spud format is an SPUD container: a 16-byte header followed by FourCC-tagged chunks. Readers skip chunks they don't know, so formats can grow without breaking older readers.

All values are little-endian.

```
FileHeader (16 bytes)
  u32  magic        "SPUD"
  u32  kind         FourCC naming the format ("MESH", "WRLD", ...)
  u32  version      per-kind version
  u32  chunk_count

chunk_count x
  ChunkHeader (16 bytes)
    u32  id         FourCC
    u32  reserved   0
    u64  size       payload bytes (excluding padding)
  payload[size]
  zero padding to the next 16-byte boundary
```

## Conventions

- Arrays are stored as a chunk whose payload is a packed array of fixed-size records; the element count is `size / sizeof(record)`.
- Strings live in a `STRS` chunk: NUL-terminated UTF-8, deduplicated, referenced by byte offset. `0xFFFFFFFF` means "no string".
- Struct layouts are defined in the C++ headers under `src/formats/` with `static_assert`ed sizes; those headers are the source of truth.
