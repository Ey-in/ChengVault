# Manifest v1 — shared capture/repository/archive contract

`fs::capture(plan, sink)` sends raw data blocks of at most 1 MiB to the sink.
The sink returns an opaque ID; repositories may use keyed hashes and standalone
archives use SHA-256. `fs::restore(manifest, source, ...)` fetches the same raw
bytes by that ID. Archives and repositories MUST authenticate/decode their own
stored blocks before returning them. All lengths and offsets are signed 64-bit
JSON integers, not C++ struct images.

The top object contains `version:1`, `createdUtc`, `entries`, `fileCount`,
`totalBytes` (logical ordinary file sizes), `storedDataBytes` (allocated stream
bytes before deduplication), and `warnings`. Repositories may add `snapshot`.

Each entry has a `/`-separated relative `path`, `type`, `size`, `metadata`,
and `streams`. Type is `directory`, `file`, `symlink`, `junction`, or `hardlink`.
Paths begin with the basename of their selected source. Duplicate root names and
Windows case-insensitive collisions are errors. Parent directories are explicit.

Streams are objects `{name,size,sparse,ranges,chunks}`. The unnamed file stream
has `name:""`; ADS names use Windows syntax `:name:$DATA`. Directories can have
named streams but no unnamed stream. Every chunk is
`{id,offset,length,sha256}`; `sha256` is the lowercase SHA-256 of raw chunk bytes.
All chunk references, including ADS, are at **entries[].streams[].chunks[].id**.
Offsets are increasing and disjoint. Non-sparse streams cover the full length;
sparse streams omit unallocated intervals. `ranges` holds `{offset,length}`
allocated intervals. Restore sets sparse allocation, logical length, then writes
only chunks. It verifies checksums before creating destination files.

Metadata stores `created`, `accessed`, `modified` as decimal FILETIME strings,
`attributes` as Win32 attribute bits, `ownerSid`, `ownerAccount`, `groupSid`,
`securityControl`, `daclNull`, and `daclBase64` (raw ACL, not a self-relative
security descriptor). No SACL claim is made. File/stream access and ACL failures
are explicit. strictMetadata=false permits metadata warnings but never ignores
data errors. Times/ACLs of directories are restored after children.

Symbolic links and junctions contain `target`, `targetDirectory`, and
`reparseBase64`; the latter preserves the exact mount-point reparse payload.
Hardlinks contain `target` referring to an ordinary-file path in this manifest.
They do not repeat streams: data and ADS are shared with the target. Selective
restore includes the canonical target when a hardlink is selected.

## Filter keys (all optional)

* `pathInclude`, `pathExclude`, `nameInclude`, `nameExclude`: string arrays of
  case-insensitive glob patterns. A non-glob path also matches descendants.
* `types`: object types; `extensions`: suffixes, with or without a leading dot.
* `owners`: Windows account names or SID glob patterns.
* `timeField`: `created`, `modified` (default), or `accessed`; `timeFrom`,
  `timeTo`: inclusive ISO8601 date-times.
* `minSize`, `maxSize`: inclusive logical size bounds in bytes.
* `attributesAll`, `attributesNone`: required/forbidden attribute bitmasks.
* `maxDepth`: depth from the selected root, which is depth 0.
* `contentContains`: UTF-8 byte substring in the ordinary unnamed stream.
* `changedSinceManifest`: previous manifest object; compare path, type, size,
  last-write timestamp and attributes. This is a metadata change filter, not a
  claim that unchanged timestamps prove unchanged bytes.

Exclusions win; different conditions combine with AND. Ancestor directories of
matching objects are retained. A path/name exclusion on a directory prunes its
subtree. Preview returns entries plus `selected` and human-readable `reason`.

Restore rejects traversal, reserved device names, reparse ancestors, malformed
ADS, file/parent conflicts, duplicate case-folded paths, invalid chunks, and
hardlinks to non-files. Default is no overwrite. File replacement is atomic per
file, not a global transaction across an entire restored tree.
