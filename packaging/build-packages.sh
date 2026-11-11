#!/bin/sh
# build-packages.sh VERSION: build steam-scale and write a .deb and an .rpm of it into build/packages.
# Needs the build requirements from docs/development.md, plus curl. Run it on the oldest distribution the packages
# should install on, because the library needs at least the C library version it was built against.
set -eu
version=${1:?usage: build-packages.sh VERSION}
nfpm_version=2.48.0
nfpm_sha256=6d7f9a28b2f397934403950fcbba83f598b08d02edbe79a0815b3b4897341750

cd "$(dirname "$0")/.."
make
make install PREFIX=/usr SHADOWDIR= DESTDIR=build/stage

mkdir -p build/nfpm build/packages
curl -fsSL -o build/nfpm/nfpm.tar.gz \
    "https://github.com/goreleaser/nfpm/releases/download/v$nfpm_version/nfpm_${nfpm_version}_Linux_x86_64.tar.gz"
echo "$nfpm_sha256  build/nfpm/nfpm.tar.gz" | sha256sum -c -
tar -xzf build/nfpm/nfpm.tar.gz -C build/nfpm nfpm

for packager in deb rpm; do
    VERSION=$version build/nfpm/nfpm package -f packaging/nfpm.yaml -p $packager -t build/packages
done
ls -1 build/packages
