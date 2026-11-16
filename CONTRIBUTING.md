# Contributing

Thank you for your interest in Jotter. Bug reports, ideas and pull requests are welcome.

## Reporting problems and ideas

Open an [issue](https://github.com/nir0k/Jotter/issues/new/choose) with the bug report or feature request form. For a bug, include the Jotter version, the distribution and desktop, the steps that reproduce it and what you expected. Vulnerabilities are reported privately as described in [SECURITY.md](SECURITY.md).

## Pull requests

For anything larger than a small fix, open an issue first so that the change can be discussed before you spend time on it. Changes to the library format or synchronisation also change [docs/spec.md](docs/spec.md), which other clients of a library follow, and need agreement in the issue.

Before opening a pull request:

- Keep it to one topic; unrelated refactoring, renaming or formatting goes in a pull request of its own.
- Build and test as described in [docs/development.md](docs/development.md): the `dev` preset builds without warnings, `ctest --preset dev` passes, the `lint` preset reports no clang-tidy findings and `clang-format` reports no changes.
- Add or update tests for changed behaviour and a regression test for a fixed bug where possible. Changes to synchronisation are also checked against the sync test stand.
- Write code, comments, documentation and interface text in English. New or changed interface strings are translated in `app/i18n/jotter_ru.ts`; `ctest` fails while a translation is missing.
- Update the documentation that the change affects.

By submitting a pull request you agree that your contribution is licensed under the [Apache License 2.0](LICENSE).
