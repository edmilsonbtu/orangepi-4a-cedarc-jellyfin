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

License and third-party code provenance review pending.
Do not assume that all components are available for redistribution
under a single license.
