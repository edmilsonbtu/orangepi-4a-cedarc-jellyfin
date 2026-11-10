#!/usr/bin/env python3
import os
import sys

V607 = "/opt/cedar-native/v6.0/cedar-adapter-v6.0.7.1.py"
V608 = "/opt/cedar-native/v6.0/cedar-adapter-v6.0.8.py"
V54 = "/opt/cedar-native/beta54/cedar-adapter-fmp4-v5.4.py"
LEGACY = "/opt/cedar-native/jellyfin-test/cedar-adapter.py"

args = sys.argv[1:]

def option(name):
    try:
        i = args.index(name)
        return args[i + 1]
    except (ValueError, IndexError):
        return None

if os.environ.get("CEDAR_V6_JELLYFIN") == "1" and option("-hls_segment_type") == "mpegts":
    os.execv(sys.executable, [sys.executable, V608, *args])

if os.environ.get("CEDAR_TV_BETA") == "1":
    os.execv(sys.executable, [sys.executable, V54, *args])

os.execv(sys.executable, [sys.executable, LEGACY, *args])
