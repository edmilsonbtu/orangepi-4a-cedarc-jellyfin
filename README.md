# Orange Pi 4A CedarC Jellyfin Integration

Experimental hardware-accelerated video processing integration for
Jellyfin on the Orange Pi 4A (Allwinner T527).

## Reference environment

- Board: Orange Pi 4A
- SoC: Allwinner T527
- Base image: AurealNix CLI v0.5
- OS: Debian 13 (Trixie)
- Kernel: 6.18.55-g3f67628a8bff
- Architecture: aarch64
- Python: 3.13.5
- PyAV: 19.0.2
- Hardware interface: /dev/cedar_dev

## Architecture

Jellyfin
  -> FFmpeg wrapper
  -> Cedar selector
  -> V6.0.8 adapter
  -> V5.4 process controller
  -> V5.4 feeder
  -> CedarC native muxer
  -> FFmpeg / HLS output

The integration supports fallback to Jellyfin's original FFmpeg
when the CedarC path is not selected.

## Build

On a compatible system with CedarC development headers and libraries:

    ./scripts/verify.sh
    ./scripts/build.sh
    ./scripts/package.sh

The native muxer is compiled from source.

## Important limitations

- Experimental software; not production-ready.
- Requires a compatible CedarC driver and userspace libraries.
- Installation and runtime paths are currently specific to the
  reference environment.
- Not all codecs, resolutions, subtitle formats, or HLS modes
  have been validated.
- Hardware transcoding and FFmpeg remuxing are distinct operations.
- The distribution package does not include the required
  Python virtual environment or vendor libraries.
- Do not replace a working Jellyfin installation without testing.

## Upstream

Reference base image:
https://github.com/ut-slayer/orangepi-4a-mainline

## License

Original project code: MIT License.

All included source files were developed for this project with AI
assistance. External dependencies retain their respective licenses.
See docs/THIRD-PARTY.md.

## Reference operating system — AurealNix

Development and hardware validation were performed on the
AurealNix Debian 13 CLI v0.5 image for the Orange Pi 4A.

- Project: [Orange Pi 4A Mainline Linux](https://github.com/ut-slayer/orangepi-4a-mainline)
- Maintainer: Juan Manuel López Carrillo (AurealNix)
- Hardware: Orange Pi 4A / Allwinner T527
- Reference kernel: Linux 6.18.55
- Distribution: Debian 13 (Trixie)

The AurealNix project provides the Linux kernel integration,
device-tree configuration and CedarC VPU driver support used
in the reference test environment.

This repository provides a separate, independently developed
CedarC transcoding integration for Jellyfin.

No AurealNix kernel patches or operating-system images are
redistributed by this repository.

Special thanks to Juan Manuel López Carrillo and the
AurealNix contributors for their Orange Pi 4A work.
