# Listopad++

<p align="center">
  <img src="assets/listopad-plus-plus.png" width="64" height="64" alt="Listopad++ icon">
</p>

Listopad++ is a fast, native, fully offline text editor for Windows 10 2004+
and Windows 11. It is written in C++20/Win32, has no background process,
telemetry, news, ads, updater, or network code.

## Implemented v1 features

- one instance per Windows user, Unicode named-pipe forwarding, tabs, long/UNC paths;
- statically embedded Scintilla 5.6.4 and Lexilla 5.5.1, folding, line numbers,
  light/dark system theme, automatic language detection and manual access to
  every built-in Lexilla lexer;
- hybrid HTML highlighting that keeps markup, JavaScript and CSS-in-`style`
  token classes separate, plus local BSL (`.bsl`) and OneScript (`.os`)
  highlighting with Cyrillic identifiers and no language server;
- UTF-8 internal text with strict BOM/UTF detection, uchardet fallback, manual
  reopen as UTF-8/UTF-16/Windows-1251/1252/CP866, and source encoding/EOL preservation;
- strict JSON, XML and conservative HTML formatting as one undo operation;
- Emmet 2.4.11 on a lazily created, memory-limited QuickJS-NG runtime;
- PCRE2 10.47 UTF/UCP/JIT search and replacement in a non-modal panel, including
  whole words, wrap, selection/all-tabs scope, cancellation and resource limits;
- directory watching with explicit Reload/Keep conflict handling and guarded,
  atomic saves;
- memory-mapped, virtualized read-only mode for files from 128 MiB (configurable),
  with asynchronous text/regexp search;
- classic HKCU context-menu registration for portable builds and an isolated
  `IExplorerCommand` DLL plus sparse identity manifest for the Windows 11 menu;
- session-long UAC save broker: the editor stays unelevated and keeps all tabs open.

The intentionally deferred features are directory search, hex view, diff/merge,
plugins, LSP, completion, crash recovery, session restore and auto-update.

## Command line

```text
ListopadPP.exe [--line N[:M]] [--encoding NAME] <file...>
ListopadPP.exe --register-context-menu
ListopadPP.exe --unregister-context-menu
```

Common encodings include `utf-8`, `utf-8-bom`, `utf-16le`, `utf-16be`,
`windows-1251`, `windows-1252` and `cp866`.

## Building

Requirements: Windows x64, Visual Studio 2022 Build Tools with MSVC and a
Windows 10/11 SDK, CMake 3.29+, Ninja, Git and PowerShell 7 or Windows PowerShell.
Dependencies are locked by `vcpkg.json`; Scintilla and Lexilla are fetched at
verified revisions by CMake.

```powershell
./scripts/bootstrap-vcpkg.ps1
./scripts/build.ps1 -Preset debug
./scripts/build.ps1 -Preset release
```

The direct equivalent is:

```powershell
$env:VCPKG_ROOT = "$PWD/.deps/vcpkg"
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
```

## Packaging and signing

Install WiX v4 (`dotnet tool install --global wix`) to produce MSI in addition
to the portable ZIP and sparse identity MSIX:

```powershell
./scripts/package.ps1 -Version 0.1.0
```

Unsigned builds run normally, but Release UAC saves and the modern Windows 11
context menu are deliberately disabled. An unsigned MSI installs the classic
fallback without attempting sparse-package registration. For local development,
create a test certificate explicitly and package with it. Certificate creation
shows a UAC prompt because MSIX deployment requires the public development
certificate in the local machine `TrustedPeople` store:

```powershell
./scripts/new-dev-certificate.ps1
$password = ConvertTo-SecureString 'listopad-dev-only' -AsPlainText -Force
./scripts/package.ps1 -PfxPath ./.deps/signing/ListopadPP.Development.pfx -PfxPassword $password
```

Production CI should pass a certificate whose subject exactly matches
`-Publisher`, or provide an external signing wrapper through `-SignCommand`.
The same signer must be applied to `ListopadPP.exe`, `ListopadElevated.exe`,
`ListopadShell.dll`, the identity MSIX and the MSI.

Portable users can choose **Tools → Register classic context menu**. The command
writes only under `HKCU\Software\Classes`, uses the selected Listopad++ UI
language for its title, and unregister removes that tree. The modern Windows 11
command is supplied by the signed sparse identity package installed by the MSI.

## Settings

Settings are JSON at `%LOCALAPPDATA%\Listopad++\settings.json`. If
`portable.flag` exists beside the executable, `settings.json` is stored there.
Supported keys include `uiLanguage` (`ru`/`en`), `theme` (`system`/`light`/`dark`),
`fontFace`, `fontSize`, `indentSize`, `indentWithTabs`, and
`largeFileThreshold` (bytes).

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/PUBLISHING.md](docs/PUBLISHING.md), and [SECURITY.md](SECURITY.md) for
design, repository governance, and trust-boundary details.

Contributions are accepted through pull requests; see
[CONTRIBUTING.md](CONTRIBUTING.md).
