# Repository workflow

- The `main` branch is protected. Never push commits directly to `main`.
- Put repository changes on the `dev` branch and push `dev` to `origin`.
- Open a pull request from `dev` into `main` and wait for its required checks.
- Do not merge the pull request until the user explicitly asks for the merge.
- Whenever the user asks to release a new application version, update
  `CHANGELOG.md` before the release with all changes made since the most recent
  released version.
- Create and publish a release tag only after the pull request has been merged into
  `main` and the user explicitly asks for that release. Never tag a release from
  `dev`.
