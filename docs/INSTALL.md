# Installation - Technical Preview

## Reference platform

- Orange Pi 4A
- Allwinner T527
- Debian 13 / AurealNix CLI v0.5
- ARM64
- CedarC driver exposing /dev/cedar_dev
- Compatible CedarC userspace libraries and headers
- Jellyfin FFmpeg
- Python 3.13 and PyAV 19.0.2

## Build

Run:

    ./scripts/verify.sh
    ./scripts/build.sh
    ./scripts/package.sh

## Installation

The installer defaults to dry-run mode:

    ./scripts/install.sh

Installation into a separate test prefix:

    sudo ./scripts/install.sh --apply \
      --prefix /opt/cedar-native-preview

The installer does not modify the Jellyfin service, FFmpeg
configuration, or existing production installation.

## Runtime configuration

The distribution supports configurable runtime paths.

Example:

    export CEDAR_ROOT=/opt/cedar-native-preview
    export CEDAR_PYTHON=/path/to/venv/bin/python

The Python environment must provide PyAV 19.0.2.

Vendor CedarC libraries, headers and the kernel driver
must be installed separately.

## Limitations

- The preview installer does not configure Jellyfin.
- The preview installer does not install Python or CedarC dependencies.
- Runtime execution requires CEDAR_ROOT and CEDAR_PYTHON.
- Clean-install hardware validation is still pending.
- Only the reference hardware pipeline has been homologated.
- Do not point a production Jellyfin service to the preview installation.
