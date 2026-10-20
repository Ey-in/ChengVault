# ChengVault engineering contract

Implement the user's approved Windows C++20 / Qt6 course project.
All scored archive/compression/crypto/erasure-code algorithms must be written
independently. Never copy Aegis, course sample implementations, or third-party code.
Qt/Win32 may provide UI, JSON, I/O, networking, events, threads, entropy and DPAPI
credential storage. Do not call Qt hashes, qCompress, OpenSSL, archive libraries,
or external programs for a scored runtime algorithm.

Keep modules separate. Explain algorithm invariants, formats and failure handling
in useful comments. Check integer lengths and paths before writing. Never delete
an existing destination before validated replacement. Never log passwords or keys.
Tests must exercise actual behavior and standard vectors, not replicate code.
Record actual results and limitations; do not fabricate grades or human work.

Parallel work: respect the ownership assigned in the chat, do not stage or commit
other agents' files. Public headers are the integration contract; discuss changes.
