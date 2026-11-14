# kit package manager (`forge`)

A small package manager written in C. It is built and run **outside** ucraft
(on the host) and acts on the root filesystem with `--root` (default `$UCRAFT`).
It implements, in a minimal form, phases 1 to 3 of `docs/plan-gestor-paquetes.md`.

```
make            # host binary, for development   -> pm/build/kit
make static     # static build with the ucraft toolchain (needs ucraft-env.sh loaded)
```

## Usage

```
source ~/Laboratory/ucraft/ucraft-env.sh
kit=~/Laboratory/ucraft/pm/build/kit

# build and inspect
$kit info attr             # reads forge/attr/forge.toml
$kit build attr            # downloads, builds against $UCRAFT, writes cache/pkg/attr-2.6.0_1-x86_64.ucraft
$kit hash file.tar.xz      # sha256, for writing recipes

# install into a root (--root, or $UCRAFT)
$kit init                  # create the system layout and base /etc files (never overwrites)
$kit install php           # a name: resolved with its dependencies (repositories first, then the cache)
$kit install ./x.ucraft    # a package file
$kit list | files attr | owns /usr/bin/getfattr | check attr
$kit remove attr

# repositories
$kit repo build repo/      # make a repository folder from the packages in the cache
$kit sync                  # download the index of every repository in <root>/etc/ucraft/repos
$kit search php            # search the index
$kit upgrade               # upgrade what has a newer version in the index

# container images
$kit image build examples/min/ucraft.toml
```

Without `--root` or `$UCRAFT` it will not install or remove anything (this prevents touching the host's `/` by
mistake). Inside a ucraft system (`ID=ucraft` in `os-release`) the root is `/` and the cache is `/var/cache/ucraft`.
`build` uses `--root` as the sysroot: it compiles against the headers and libraries that are already there.

## System layout

The directory layout and the base `/etc` files (`passwd`, `group`, `shadow`, `profile`, `shells`, `protocols`,
`services`, `os-release`...) are **not a package**: they are a template inside `kit` (`pm/src/layout.c`),
applied by `kit init` and by `kit image build`. It only creates what is missing. To change a default, edit
the tables in `layout.c`.

## Repositories

A repository is a folder with an `index` and the `.ucraft` files, served over HTTP (or just a local folder).
`/etc/ucraft/repos` lists one URL per line; `--repo URL` overrides it. See `docs/distribution.md` for how it
works, how to serve it locally and through a Cloudflare tunnel, and what is protected today (sha256 of every
package, but the index itself is not signed yet).

## Container images

`ucraft.toml` has at most these keys: `name`, `profile` (`min` by default: musl, busybox and ucraft; `bare` has no
package manager), `packages`, `copy` (`"source:/destination"`), `cmd` and `user` (`nobody` by default).

```
name     = "hello-php"
packages = ["php"]
copy     = ["./app:/app"]
cmd      = ["php", "-S", "[::]:8080", "-t", "/app"]
```

## Where things live

| What | Where |
| --- | --- |
| Recipes ("forge") | `forge/<name>/forge.toml` (`--forge`, `$UCRAFT_FORGE`); `forge-local/` overrides it |
| Downloaded sources, packages, build trees, index | `cache/{src,pkg,build,index}` (outside the rootfs) |
| Database of installed packages | `$UCRAFT/usr/lib/pkg/db/<package>/{info,files,reason}` |
| Repository list | `<root>/etc/ucraft/repos` |

## Recipe format

Allowed keys (an unknown key is an error with file and line):
`name version revision description license style depends makedepends allow_var`,
`[[source]] url sha256`,
`[build] args make_args install_args install_target subdir patches skip_build`,
`[[step]] phase action args`.

Build keys:
- `patches = ["fix.patch"]`: files in the recipe folder, applied with `patch -p1` right after extraction.
- `skip_build = true`: run only the install step (e.g. kernel headers).
- `install_target` / `install_args`: change the install target or add arguments
  (e.g. `CONFIG_PREFIX=${DEST}` for BusyBox, `INSTALL_ROOT=${DEST}` for PHP).
- `subdir`: folder inside the source tree that holds the build root (cmake).
- `allow_var = true` (top level): lets a package ship `/var` (only the base skeleton package).

Styles:
- `autotools`: out-of-tree build; adds `--prefix`, `--host` and the standard directories.
- `configure`: the program's own `configure`, run in the source tree with only the recipe's `args`.
- `make`: plain `make` / `make install` in the source tree.
- `cmake`: out of tree with Ninja (`-DCMAKE_INSTALL_PREFIX=/usr`, cross defaults from `$CC`/`$CXX`).
- `none`: no build, only steps.

Step actions: `rm move link patch mkdir chmod install`.
Phases: `post_extract`, `pre_install`, `post_install` (`pre_install` also runs for style `none`).
Variables: `${DEST} ${SRC} ${JOBS} ${VERSION} ${NAME} ${TARGET}`.
`UCRAFT_JOBS=4` limits build parallelism on machines with little RAM.

## What it does, and what is missing

Done: own strict TOML parser (a subset), own sha256 and tar, downloads with `curl`,
shell-free builds (`posix_spawn`), package `/etc` moved to `usr/share/factory/etc`,
`/var` forbidden, `*.la` removed, two-phase install with atomic `rename` and sha256
verification, file conflict detection, upgrades, direct dependency checks, `check`, `owns`.

Not yet: USE flags, `world`/`autoremove`, zstd compression of the package (today it is an uncompressed tar),
SONAME-based dependencies, git recipe sources, signed repository index, `unshare`, remounting `/usr` read-write.
