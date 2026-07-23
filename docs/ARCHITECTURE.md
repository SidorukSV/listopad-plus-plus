# Architecture

```text
Explorer / command line
        │ versioned OpenFilesRequest
        ▼
ListopadPP.exe (unelevated, one process per user)
  ├─ Win32 shell window, tabs and status UI
  ├─ Scintilla + all static Lexilla lexer modules
  ├─ UTF-8 documents / encoding and EOL metadata
  ├─ ReadDirectoryChangesW watchers
  ├─ cancellable PCRE2 workers
  ├─ lazy QuickJS-NG + fixed Emmet bundle
  ├─ shared-document Scintilla DocumentMap
  ├─ memory-mapped LargeFileView
  └─ memory-mapped HexViewWindow
        │ authenticated SaveRequest, only after ACCESS_DENIED
        ▼
ListopadElevated.exe (session broker, minimal command set)
```

`listopad_core` owns deterministic, testable services: command-line and IPC
serialization, encoding, document loading, fingerprinted atomic I/O, PCRE2,
formatters, Emmet and settings. It contains no editor window.

`ListopadShell.dll` implements only `IExplorerCommand` and `IClassFactory`. It
enumerates selected `IShellItem` paths and starts `ListopadPP.exe`; no editor,
lexer, parser or JS runtime is loaded into Explorer.

Each tab has one active `ViewKind`: editable Scintilla text, read-only
`LargeFileView`, or read-only `HexViewWindow`. Large text and hex views use a
file mapping and paint only visible rows. Their searches run against the mapping
on cancellable workers. Editing, replacement, formatting and Emmet are disabled
in these modes.

Editable tabs may also own a narrow Scintilla `DocumentMap`. It shares the
editor document and styled bytes but owns its visual styles and scrolling. The
map never marks the shared document read-only; its subclass rejects input and
uses mouse gestures only for navigation.

Built-in language metadata is the single source for extension detection,
Save As filters, and the canonical extension appended to a new file. Raw
Lexilla lexers without metadata remain available, but do not guess a suffix.

Normal saves are optimistic transactions guarded by a strong fingerprint
(volume serial, 128-bit file ID, length and last-write time). External directory
events are only hints; the fingerprint decides whether the document diverged.

Runtime-heavy components are demand driven. QuickJS is created on the first
Emmet action; formatters are called only from the format command; the UAC broker
starts only after an access-denied save.
