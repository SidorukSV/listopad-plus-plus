# Third-party notices

Listopad++ statically embeds or links the following components. Exact license
texts are copied to the `licenses` directory of each release by the packaging
script.

| Component | Pinned version | License |
|---|---:|---|
| Scintilla | 5.6.4 | Scintilla license |
| Lexilla | 5.5.1 (`f37af225`) | Scintilla license |
| PCRE2 | 10.47 | BSD-3-Clause |
| pugixml | 1.16 | MIT |
| yyjson | 0.12.0 | MIT |
| tidy-html5 | 5.8.0 | W3C-style |
| QuickJS-NG | 0.15.1 | MIT |
| uchardet | 0.0.8 | MPL-1.1 / tri-license notices in distribution |
| Emmet | 2.4.11 | MIT |
| Catch2 (tests only) | 3.15.2 | BSL-1.0 |

The generated Emmet bundle is built from the official `emmet` npm package by
`tools/emmet/build.mjs`; it contains no network or update code.
