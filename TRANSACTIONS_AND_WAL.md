# Transactions, WAL and the change journal

The internal Engine uses a single, exclusive transaction lease for serializable visibility. The lease carries a session owner and a unique in-process ticket. Begin, reads, edits, commit, rollback and savepoints verify that ticket. A wait can time out or be cancelled. Disconnect and expiry discard access to private edits and release the lease. Transaction and wait limits are at most 30 seconds. Work already in a synchronous Windows I/O call reaches its next boundary before the lease can be released.

Each transaction keeps up to 256 private page images, 4096 changes and 2 MiB of row change data. Up to eight savepoints preserve the dirty map and change boundary. Rolling back to a savepoint restores both together. Crossing the budget fails the operation. There is no disk spill. A private row ID becomes stable when its transaction commits.

## WAL format 1

The little-endian segment header is 64 bytes. It begins with `SDPWAL1\0`, version, header size and initial LSN, followed by zeroed reserved bytes and a CRC-32. The file name ends with `.wal.` and the 20-digit first LSN. Each record starts with a 32-byte header. It includes magic `SDPR`, byte length, LSN, page image count, journal count, resulting data page count and header CRC. The payload stores ordered page ID/full-page image pairs followed by each change's row ID, row/catalog kind, before and after image lengths and image bytes. Each row image carries its schema version. A zero length denotes an absent version. Maximum record length is 4 MiB and the retained WAL quota is 64 MiB.

The final 24-byte `SDPC` marker repeats the record size and LSN and covers the record and marker with CRC-32. Commit writes the WAL body, writes its marker and calls Windows `FlushFileBuffers`. Publication then applies post-images and flushes the data file. The API returns success after those boundaries. The journal remains in the WAL as part of the same commit, including row or catalog before/after images. A WAL write failure before the marker can be rolled back to the previous byte boundary. A marker or flush error can leave the outcome uncertain. Publication failure also pauses the engine for recovery. Quota exhaustion rejects writes before a new marker. [Windows FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

The lease, WAL and journal are exercised through the internal C++ API. Server v1 continues to advertise SQL and network transactions as unavailable. Recovery and checkpoint behavior are documented separately.
