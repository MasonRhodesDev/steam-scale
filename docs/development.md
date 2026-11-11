# Development

## Build

You need a C compiler, make, pkg-config, wayland-scanner, wayland-protocols, and the development files of
libxcb (with RandR) and libwayland-client.

    make
    sudo make install                    # into /usr/local
    make install PREFIX=$HOME/.local     # without root

## The steam link

The install puts a `steam` link to `steam-scale` in the install directory. When that directory comes before
the real `steam` on `PATH`, the name `steam` starts steam-scale. steam-scale then starts the next `steam` on
`PATH` and passes its arguments on. The packages put the link in `/usr/local/bin`.

To install no link, run `make install SHADOWDIR=`. To put the link in another directory, run
`make install SHADOWDIR=/some/dir`.

A launcher that starts Steam by its full path, such as the stock menu entry, does not go through the link.
`steam-scale --install-desktop-entry` writes a menu entry for your user that does.

Steam prints one `wrong ELF class` warning from `ld.so` at startup. Steam's first process is 32-bit and cannot
load the 64-bit library. The warning is harmless, because the process that draws the windows is 64-bit.

## Packages

| Package | Built from | Built by |
|---------|------------|----------|
| Arch | `packaging/PKGBUILD` | `makepkg` |
| `.deb` and `.rpm` | `packaging/nfpm.yaml` | `sh packaging/build-packages.sh VERSION` |

`build-packages.sh` downloads [nfpm](https://nfpm.goreleaser.com) and writes both files to `build/packages/`.
CI runs it on Debian 12, so the library loads on any distribution with glibc 2.34 or later. The `.rpm` names
Fedora's dependencies. It is untested on openSUSE.

## Release

Commit subjects on `main` decide the version, following [Conventional Commits](https://www.conventionalcommits.org).
A push with a `feat`, `fix`, `perf` or `revert` commit since the last tag releases a patch version. A breaking
commit (`type!:` or a `BREAKING CHANGE:` footer) releases a minor version. From 1.0.0 on, breaking is major,
`feat` is minor and the rest are patch. Other types, such as `docs` and `ci`, release nothing.

The `Tag` workflow creates the tag and starts the `Release` workflow, which builds every package with the tag
as its version and attaches them to a GitHub Release. No file holds the version.
