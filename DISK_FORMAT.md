# Storage format 1

`store.sdp` uses 4096-byte pages and unsigned little-endian numbers. All offsets are relative to the start of a page. CRC-32 uses polynomial `0xedb88320`. Bytes 4092–4095 contain the checksum of bytes 0–4091. A checksum failure is reported as corruption.

| Offset | Header page 0 field |
| ---: | --- |
| 0 | `SDPST01\0` magic |
| 8 | 32-bit format version, 1 |
| 12 | 32-bit page size, 4096 |
| 16 | 32-bit page count including header, 1–65536 |
| 24 | 64-bit latest published LSN |
| 4092 | CRC-32 |

| Offset | Data page field |
| ---: | --- |
| 0 | `SDPG` magic |
| 4 | 16-bit type. 1 free, 2 heap, 3 overflow. |
| 6 | 16-bit header size, 32 |
| 8 | 32-bit page ID, its file page number |
| 12 | 64-bit page LSN |
| 20 | 16-bit heap slot count |
| 22 | 16-bit heap payload start or overflow payload length |
| 24 | 32-bit next overflow page ID, zero at end |
| 4092 | CRC-32 |

Unused header fields are zeroed. Heap slots are 16 bytes each and grow from offset 32. The slot stores the inline offset and length as two 16-bit fields, total length as 32 bits and the first overflow page ID as 32 bits. The last four bytes are reserved. Inline data grows downward from offset 4092 and is repacked when rows change. Up to 512 bytes per record are inline. Overflow chaining stores the remaining bytes, within the same 512 KiB total row limit. Deleting a record returns the overflow pages to the allocator. An empty slot is a tombstone. Row ID is `page_id << 32 | (slot_index + 1)`. Slot indices are preserved and are independent of any primary key.

Each encoded row contains a 16-bit column count, a 16-bit null bitmap size, a nonzero 32-bit schema version, and the null bitmap. The bitmap preserves NULL separately from empty text or binary. Non-null entries carry tags 1 (signed 64-bit integer), 2 (scale byte and exact decimal coefficient), 3 (length-prefixed UTF-8) or 4 (length-prefixed binary). The decimal syntax and types match the [wire v1 type mapping](contracts/v1/TYPE_MAPPING.md). Row decoding validates lengths, offsets, UTF-8, decimal syntax and the bitmap.

## Operations

```powershell
.\StarDataParadiseServer.exe --config .\server.json --initialize-storage
.\StarDataParadiseServer.exe --config .\server.json --check-storage
```

Initialize explicitly in an unused data directory. A directory with only the administrator verifier and ownership marker qualifies. The storage file is created exclusively and existing files trigger an error. Each operation acquires the Server data-directory lock. Starting Server with an existing store scans the header, checksums, page metadata, heap rows and overflow references. The read-only integrity command reports counts derived from the file.

The internal page cache is bounded at 1–64 pages. Short I/O, permission errors, invalid format and corrupt storage produce explicit errors. The initial storage API is exercised with isolated fixtures. WAL and crash recovery are covered in subsequent format documents. SQL remains unavailable.
