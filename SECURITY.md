# Security model

Listopad++ does not make network requests and does not load plugins. Untrusted
file contents are processed in-process, so parser and lexer dependencies remain
part of the normal desktop-app attack surface and should be updated deliberately
after review.

## Elevated save broker

`ListopadElevated.exe` has one operation: atomically replace a canonical file
with caller-supplied bytes. It cannot start processes, alter ACLs, manipulate
services, or execute content.

The editor creates a byte-mode named pipe whose name contains its PID and a
128-bit CSPRNG nonce. The pipe rejects remote clients and has a protected DACL.
After UAC elevation both sides verify the other pipe endpoint PID. The broker
also verifies the parent image name and directory. Release builds require valid
offline Authenticode policy for both binaries and an identical leaf signing
certificate.

Each versioned request includes a monotonically increasing request id, canonical
path, expected volume/file ID/size/write-time fingerprint, byte length and
SHA-256 digest. Metadata and content lengths are bounded. The broker recomputes
the digest, rechecks the fingerprint immediately before replace, writes a
sibling temporary file, flushes it, then uses `ReplaceFileW` or a write-through
move. It exits when the pipe or editor closes.

Explicit Overwrite accepts the disk version visible when that save begins; it
does not authorize overwriting another change that arrives while the temporary
file is being written.

## Reporting vulnerabilities

Do not include sensitive file contents, signing keys or PFX passwords in a
public report. Provide a minimal reproducer, affected version and Windows build
to the project maintainers through a private security advisory.
