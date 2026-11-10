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

## Limitations

The current integration contains absolute paths pointing to
/opt/cedar-native. Installing into a different prefix does not
automatically make those scripts executable.

The test installation is intended for file layout validation only.
Runtime relocation and clean-install tests are still pending.

Do not point a production Jellyfin service to the preview installation.
