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
  └─ memory-mapped LargeFileView
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

Large files use a read-only file mapping and paint only visible lines in a
custom child window. Search runs against the mapping on a cancellable worker.
Editing, syntax highlighting, replacement, formatting and Emmet are disabled in
this mode.

Normal saves are optimistic transactions guarded by a strong fingerprint
(volume serial, 128-bit file ID, length and last-write time). External directory
events are only hints; the fingerprint decides whether the document diverged.

Runtime-heavy components are demand driven. QuickJS is created on the first
Emmet action; formatters are called only from the format command; the UAC broker
starts only after an access-denied save.
