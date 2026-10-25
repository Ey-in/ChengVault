# ChengVault standalone archive formats

All numbers in project-owned binary framing are unsigned little-endian. Integer
fields are decoded explicitly; no ABI-dependent C++ object bytes are persisted.
All runtime transforms are implemented by this project.

## Outer CVARCH2 envelope

The 8-byte magic is `CVARCH2\n`, followed by a 4-byte header length and compact
UTF-8 JSON. Headers contain version=2, format=indexed|tar, compression, cipher,
and chunkSize=1048576. Encrypted headers add a random 16-byte PBKDF2 salt,
120000 iterations, and an authenticated wrapped random 32-byte master key.
PBKDF2-SHA256 runs once per open/create, not once per data block. The password
never appears in a header or log. Unencrypted archives omit key material.

Each record is a 4-byte envelope length followed by envelope bytes. Plain record
contents are sequence:u64, kind:u8, headerSHA256:32, and body. Encrypted records
use crypto::seal with independent random salts and derived encryption/MAC keys.
Unencrypted records append a SHA-256 checksum; this detects accidental damage
but is deliberately not described as authentication against an active attacker.

Data records (kind=1) contain rawLength:u32, rawSHA256:32, then an independently
compressed block. A final record (kind=0) contains totalRawLength:u64 and the
SHA-256 of the complete inner archive. Open checks sequence, exact header hash,
per-block length/digest, final length/digest and EOF. Missing, reordered, appended
or truncated records fail. Maximum envelope length is 4 MiB and decompressed
block length is at most 1 MiB. The inner stream is written to a temporary file;
no restore destination is created until envelope verification has completed.

Compression precedes encryption. Because the whole inner stream is encrypted,
file names, metadata, directory structure and file bytes are all protected.
The header reveals only format/algorithm and KDF parameters. Content is never
written into an output backup before encryption, although temporary cleartext
files are used during archive assembly/open. A classroom filesystem adversary
with access to the running account's temporary directory is outside this model.

## Indexed inner layout

`CVIDX01\n`, manifestLength:u64, manifest JSON, uniqueChunkCount:u64, then each
unique chunk: lowercase SHA-256 ID (64 ASCII bytes), rawLength:u32, raw bytes.
IDs are sorted when writing. Parsing builds an offset table and validates that
its IDs exactly match the manifest. Restore uses table offsets, including when
only selected files are restored. Duplicate data is stored once. The external
wrapper is decoded first; selection does not skip envelope authentication.

## PAX/TAR inner layout

This is a genuine POSIX PAX/USTAR stream, not a renamed custom chunk list:

* 512-byte USTAR headers with octal numeric fields and header checksums.
* Per-member PAX headers, with self-counted UTF-8 records and `path`, `size`,
  and optional `linkpath` keys. Large/Unicode paths and sizes use PAX.
* Ordinary files contain their actual logical bytes; sparse gaps are zero bytes.
* Directories use type 5, hard links type 1, and symlinks/junctions type 2.
* First member is manifest JSON, marked `CHENGVAULT.kind=manifest`.
* NTFS named streams are separate file members marked
  `CHENGVAULT.kind=stream`, `CHENGVAULT.path`, `CHENGVAULT.stream`.
* ACLs, SID ownership, timestamps, sparse ranges and exact junction information
  live in the versioned manifest. Ordinary TAR programs can read normal files;
  full NTFS restoration requires ChengVault to interpret these extensions.
* Payload padding is zero-filled to 512 bytes and the archive ends in exactly
  two zero blocks. Paths, member types, sizes and link targets must match the
  manifest before restoration.

The outer wrapper must be decoded before interoperability tests with ordinary
TAR readers. The generated inner TAR can be examined in tests independently.
On-disk source spool, raw inner archive and decoded inner archive are temporary,
bounded-memory intermediates; users need enough temporary storage. Large sparse
files are efficient in the indexed format but expand logically in TAR before
optional compression. The UI/report must explain this tradeoff.

## Failure and restoration boundaries

QSaveFile commits complete output archives atomically and never falls back to
direct overwrite. Restore authenticates all required blocks before touching the
destination, then builds each file/ADS in a sibling temporary file and renames
it after metadata handling. Replacement is per-file atomic, not a transaction
across the whole directory tree. Strict metadata failures remain failures;
optional relaxed mode reports explicit warnings. NTFS directory ADS on an
existing destination directory are rejected instead of overwritten in place.
