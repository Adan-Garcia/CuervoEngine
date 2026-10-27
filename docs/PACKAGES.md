# Packages

A package is a reusable library of Pluma scripts that games and other packages import.
Each package is a namespace of its own, so its names never clash with the game's or with
another package's. This page covers the project side: manifests, dependencies,
vendoring, versions and export. The language side (`import`, `pub`, qualified names) is in
[LANGUAGE.md](LANGUAGE.md) §9.26. `examples/garden` is a game that uses two packages:
`tween`, from `examples/libs/tween`, and the vendored `palette`.

## A package

```
tween/
  package.toml
  scripts/
    tween.plm        # any number of files, in subdirectories too
```

`package.toml`:

```toml
name = "tween"            # a Pluma identifier: the namespace code imports
version = "1.2.0"         # MAJOR.MINOR.PATCH
description = "Easing curves and value tweens"
license = "MIT"

[dependencies]            # same forms as in project.toml (below)
```

A package's files see one another's names, as a game's files do. Code outside the
package sees only its `pub` declarations:

```pluma
// tween/scripts/tween.plm
pub enum Ease { Linear, InQuad }
pub fn ease(curve: Ease, t: float) -> float { ... }
pub class Tween { ... }
fn clamp01(x: float) -> float { ... }     // private to the package
```

```pluma
// a game's script
import tween                     // or: import tween as tw
import palette.{petal, leaf}     // or bring names in unqualified

var t = tween.Tween(12.0, 48.0, 60, tween.Ease.InQuad)
```

A package's code can use the engine API and the std library. It can't see the game's
own declarations, so it works in any game.

To make and check one:

```sh
cuervo-tools package new libs/sparkle --name sparkle [--version 0.1.0]
cuervo-tools package check libs/sparkle
```

`package check` resolves the package's own dependencies and checks its scripts in its
namespace against the engine API, the way a game that uses it would.

## Tests

A package (or a game) can carry `@test` functions ([LANGUAGE.md](LANGUAGE.md) §9.27):

```pluma
// tween/scripts/tween_test.plm
@test fn curves_end_at_one() {
    assert(ease(Ease.InQuad, 1.0) == 1.0)
}
```

```sh
cuervo-tools test examples/libs/tween     # the package's tests
cuervo-tools test examples/garden         # the game's own tests (not its packages')
```

They are left out when a game runs or is exported.

## Dependencies

A game lists its packages in project.toml:

```toml
[dependencies]
palette = "1.0.0"          # vendored: packages/palette/ inside the project

[dependencies.tween]       # a package in another directory
path = "../libs/tween"     # relative to this project.toml
version = "1.2.0"
```

- **Vendored** packages live in the project under `packages/<name>/` (with their
  `package.toml` and `scripts/`). They are copied into the project and versioned with it.
- **Path** packages live in another directory, relative to the manifest that lists
  them. A package's own `path` dependencies are relative to that package.
- **Versions are pinned.** The dependency's version must equal the package's `version`
  exactly, so a project always builds against the code it was tested with. A project
  holds one version of each package. If two packages pin different versions of a third,
  that is an error that names both, and so are dependency cycles.
- Only the project's own dependencies are loaded, plus theirs (transitively).
  Dependencies load first, so their globals initialize before their dependents' and
  before the game's. Within a package, files load in path order.

`cuervo`, the editor, `cuervo-tools lsp` and hot reload all read the same list. Editing a
package's script while the game runs reloads it like any other script.

## Export

`cuervo-tools pack` and `export` put every package in the pack under
`packages/<name>/`: its `package.toml`, `scripts/` and `LICENSE` file if it has one.
Vendored packages are already there. A packed game finds its packages there and ignores
`path`, so a pack has no references outside itself. With `--scripts native` the
packages' code is compiled into the game's native module like the game's own.

A packed game reaches the same state hash as its development run, both with source
scripts and with AOT (`tests/cuervo/package_tests.cpp`, and by hand with
`examples/garden`).

## Version ranges and registries

A package can state which engine versions it works with, as a range:

```toml
engine = ">=0.1, <0.3"     # in package.toml; leave it out for any version
```

A range is a comma-separated list of clauses, all of which must hold:

- `>=1.2`, `>1.2`, `<=1.2.4` and `<2` compare;
- `=1.4.0` or a bare `1.4.0` means exactly that version;
- `^1.2` means compatible with 1.2: at least 1.2.0 and below 2.0.0 (below 0.3.0 for
  `^0.2`);
- `*` means any version.

A game whose engine is outside a package's range doesn't build, and the error names both
versions.

**Registries.** A registry shares packages, both plain script packages and plugins:

```sh
cuervo-tools plugin publish ../libs/tween --registry /srv/cuervo-registry
cuervo-tools plugin install tween@^1.2 --registry /srv/cuervo-registry   # in the game
```

`install` picks the newest version in the range that works with this engine. It vendors
the package and its dependencies under `packages/` and pins the exact version in
project.toml, so the game still builds against the code it was tested with: a project
names one version per package. `CUERVO_REGISTRY` sets a default registry.
[PLUGINS.md](PLUGINS.md) ("Sharing plugins") describes the registry, its checksums and
the checks.

**Licenses.** `cuervo-tools license report <project>` lists the packages a game uses and
their licenses. `export` writes the same table to `licenses/PACKAGES.md`, next to each
package's license text and the engine's own notices.

## Not yet

- Registries are directories (local, a shared drive, or files served as they are);
  publishing to and installing from an HTTP server is not built yet.
- Packages hold scripts and, for plugins, native libraries; assets come later.
