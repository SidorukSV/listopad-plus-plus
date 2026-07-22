# Repository and release model

Listopad++ uses one public canonical repository and a trunk-based workflow.
Public visibility does not grant write access: maintainers receive explicit
repository access, while everyone else contributes through a fork.

## Change flow

All work starts on a short-lived branch and reaches `main` through a pull
request. The `main` branch is always releasable and protected from direct
pushes, force pushes and deletion. Its required Windows x64 check builds and
tests both Debug and Release configurations.

The initial branch rule requires a pull request but no approving review, so a
single maintainer is not locked out of the project. Once a second active
maintainer is available, increase the required approval count to one and
enable required CODEOWNERS approval for release and trust-boundary files.

## Releases

The Release workflow has two equivalent entry points:

- pushing a semantic version tag such as `v0.1.1`;
- running the workflow manually and entering `0.1.1` if the tag was forgotten.

A manual run verifies that its selected commit belongs to `main`, creates the
missing annotated tag, builds and tests the exact version, packages the ZIP,
MSIX and MSI, writes SHA-256 checksums and publishes a GitHub Release. An
existing tag that points to a different commit is never moved.

The release job uses the `production` GitHub Environment. Production signing
is enabled by adding these environment values:

- variable `LISTOPAD_PUBLISHER` with the certificate subject;
- secret `LISTOPAD_PFX_BASE64` with the base64-encoded PFX;
- secret `LISTOPAD_PFX_PASSWORD` with its password.

Without those secrets, CI still creates a clearly unsigned development build;
the UAC broker and modern Windows 11 context-menu integration remain disabled
by design. Signing secrets must never be stored in the repository.
