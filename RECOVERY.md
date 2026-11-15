# Recovery and checkpoints

Server checks the store and retained WAL under the data-directory lock before it starts listening. The storage inspector checks the same consistency rules with read-only handles. Offline recovery and checkpoint commands use the configured data directory.

```powershell
.\StarDataParadiseServer.exe --config .\server.json --check-storage
.\StarDataParadiseServer.exe --config .\server.json --recover-storage
.\StarDataParadiseServer.exe --config .\server.json --checkpoint-storage
```

## Replay and integrity

Recovery reconstructs the published state in memory from contiguous WAL v1 segments and verified commit markers. It checks the LSN sequence, full-page images, allocated page identities, before/after journal continuity, heap slots, record encoding and overflow ownership before it writes. The checker can also validate candidate row references from callers preparing index support. Network index and SQL operations remain unavailable.

A partial tail in the final segment may be discarded once the preceding commits and the data file agree. A complete corrupt frame, a missing required segment, an unrecognized format or data ahead of the last committed WAL marker causes safe-stop. Validation failures leave the original bytes available. A torn page or header can be restored from the verified committed images. Recovery flushes retained WAL before it applies repairs, writes the storage header last and flushes the data file. Reopening an already repaired store leaves its bytes unchanged.

## Checkpoint and retention

Checkpoint records the published LSN and retention floor in a CRC-protected format-1 metadata page. It flushes a new metadata file before replacing the previous checkpoint and rotates the WAL into a fresh segment when the current segment has commits. The first WAL-managed write establishes a checkpoint record so a missing WAL set can be distinguished from a stage 08 store. A checkpoint alone never establishes a transaction commit. [Windows MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)

The retention floor is the minimum LSN needed by retained History, an active transaction view and optional backup or dataset readers. The checkpoint reports reclaimable segments whose last LSN precedes that floor. M2 retains the full journal and full-page image history from the initial LSN, so completed segments remain pinned. The 64 MiB WAL quota applies across segments. Write attempts that exceed it return a quota error.

Disk format 1 files from Etap 08 remain readable. WAL-managed writes require the matching WAL set. The unchanged Etap 08 and 09 fixtures are read during milestone verification.

## Crash verification

The crash suite creates independent files and kills only its own test processes. It checks these boundaries against the live storage implementation.

| Interrupt point | Expected outcome |
| --- | --- |
| WAL body written, marker absent | Keep the previously committed state and trim the incomplete tail. |
| WAL marker flushed | Restore the committed data and its journal. |
| First data page written | Repair any missing or torn images. |
| Data file flushed | Preserve the published bytes. |
| Change journal published | Preserve the published bytes and journal. |
| Checkpoint metadata flushed or published | Keep retained WAL, then verify the next commit. |

Additional cases check a torn header and page, loss of a commit marker from already published data, full corrupt WAL, a future disk version and dangling row references. The fixture reports elapsed recovery time and verifies byte-for-byte idempotence on a second replay. Windows API flush behavior was exercised with process termination. Electrical power loss and hardware cache integrity require separate verification. [Windows FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
