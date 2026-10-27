# Security policy

## Reporting a vulnerability

Report security problems **privately**, never in a public issue. Use GitHub's private
vulnerability reporting: on the repository, open **Security → Report a vulnerability**
(`https://github.com/Adan-Garcia/CuervoEngine/security/advisories/new`).

A useful report includes:

- what is affected (engine, Pluma VM, `cuervo-tools`, editor, a web export) and the
  version or commit;
- how to reproduce it: a minimal project, script, scene, pack or package;
- what an attacker gains (code execution, a crash, reading files, ...).

We confirm the report, work on a fix in a private advisory, and credit you when the fix
is released, unless you prefer not to be named. Please give us a reasonable time to fix
the problem before disclosing it.

## Supported versions

Fixes go into `main` and into the next release. There are no long-term support branches
yet.

## What counts

Problems in how the engine and tools handle **untrusted data** are vulnerabilities. For
example, a game pack, scene file, SVG, font, replay, registry index or package archive
that crashes the program, corrupts memory, or reads or writes files outside the places
it should. Also in scope: the web export's isolation in the browser, and checksum checks
that `cuervo-tools plugin install` fails to enforce.

Some things are code by design, and running them is not a vulnerability:

- **Scripts are code.** Pluma is not a sandbox: a game's scripts run with the game's
  privileges, and `@extern` (LANGUAGE.md §9.28) can call any C function.
- **Plugins are native code.** A plugin runs inside the player and the tools, with full
  privileges. Install packages and plugins only from sources you trust, as you would any
  dependency. `cuervo-tools plugin verify` checks a plugin's checksums, license and
  determinism, not its intentions.
- **Development tools trust the project.** Opening or running a project in the editor or
  with `cuervo-tools` runs its scripts and plugins.
