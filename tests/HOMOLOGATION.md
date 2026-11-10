# Orange Pi 4A CedarC — Homologation

## Platform

- Board: Orange Pi 4A
- SoC: Allwinner T527
- OS: Debian 13
- Architecture: aarch64
- Python environment: /opt/cedar-native/venv
- PyAV: 19.0.2
- FFmpeg: 8.1.3-Jellyfin

## Isolated transcoding test

Input:
- Codec: HEVC
- Resolution: 1920x1080
- Frame rate: 30 fps

Output:
- Codec: H.264
- Container: MPEG-TS
- Resolution: 1920x1080
- Frame rate: 30 fps
- Duration: 10 seconds

Results:
- Input packets: 300
- Frames encoded: 300
- Pipeline errors: 0
- Encoder initialization: 0
- Drain completed: yes
- Feeder return code: 0
- FFmpeg return code: 0
- FFmpeg decode validation: 0
- Total elapsed time: approximately 5.6 seconds
- Residual pipeline processes: none observed

## Scope

Validated using the relocatable distribution with CEDAR_ROOT
pointing to the build distribution directory.

The production Jellyfin service was not modified.

## Pending tests

- Controlled SIGTERM cancellation
- Additional input resolutions
- H.264 input transcoding
- Subtitle burn-in
- Jellyfin HLS integration
- Fresh-system installation
- Legacy feeder dependency resolution


## Controlled SIGTERM cancellation

Input:
- File: karate-kid2-hevc-5min.mkv
- Codec: HEVC
- Resolution: 1920x1080
- Frame rate: 24000/1001

Results:
- CedarC decoder initialized successfully
- CedarC encoder initialized successfully
- Encoding was active before cancellation
- Controller received SIGTERM
- Controller reported CEDAR_CONTROL_STOP=SIGTERM
- Controller return code: 143
- No matching residual processes observed

Status: PASS
