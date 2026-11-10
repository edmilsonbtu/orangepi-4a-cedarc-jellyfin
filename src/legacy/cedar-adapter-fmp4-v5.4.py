#!/usr/bin/env python3
"""Experimental fMP4 Cedar adapter v3; NEVER install as the Jellyfin wrapper directly."""
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path

REAL = '/usr/lib/jellyfin-ffmpeg/ffmpeg'
PROBE = '/usr/lib/jellyfin-ffmpeg/ffprobe'
CONTROL = '/opt/cedar-native/beta54/cedar-process-control-v5.4.py'
LEGACY = '/opt/cedar-native/jellyfin-test/cedar-adapter.py'
LOGDIR = Path('/var/cache/jellyfin/cedar-wrapper')
args = sys.argv[1:]


def fallback(reason):
    if os.environ.get('CEDAR_V3_DRY_RUN') == '1':
        print(json.dumps({'mode': 'fallback', 'reason': reason}, ensure_ascii=False))
        return 0
    os.execv(REAL, [REAL, *args])


def legacy():
    if os.environ.get('CEDAR_V3_DRY_RUN') == '1':
        print(json.dumps({'mode': 'legacy'}, ensure_ascii=False))
        return 0
    os.execv(sys.executable, [sys.executable, LEGACY, *args])


# Fail closed: only exact option families that have been audited.
VALUE_OPTIONS = {
    '-analyzeduration', '-probesize', '-i', '-map_metadata', '-map_chapters',
    '-threads', '-map', '-codec:v:0', '-codec:a:0', '-preset', '-crf',
    '-maxrate', '-bufsize', '-profile:v:0', '-level', '-x264opts:0',
    '-force_key_frames:0', '-sc_threshold:v:0', '-vf',
    '-avoid_negative_ts', '-max_muxing_queue_size', '-f', '-max_delay',
    '-hls_time', '-hls_segment_type', '-hls_fmp4_init_filename',
    '-start_number', '-hls_segment_filename', '-hls_playlist_type',
    '-hls_list_size', '-hls_segment_options', '-ss', '-loglevel',
    '-codec:v', '-codec:a', '-b:a', '-ac',
}
FLAG_OPTIONS = {'-y', '-nostdin', '-hide_banner', '-copyts'}


def parse():
    if len(args) < 2:
        raise ValueError('missing output')
    options, maps = {}, []
    i = 0
    while i < len(args) - 1:
        name = args[i]
        if name in FLAG_OPTIONS:
            if name in options:
                raise ValueError('duplicate flag')
            options[name] = True
            i += 1
        elif name in VALUE_OPTIONS:
            if i + 1 >= len(args) - 1:
                raise ValueError('missing value')
            value = args[i + 1]
            if name == '-map':
                maps.append(value)
            else:
                if name in options:
                    raise ValueError('duplicate option')
                options[name] = value
            i += 2
        else:
            raise ValueError('unsupported option: ' + name)
    options['maps'] = maps
    options['output'] = args[-1]
    return options


def seek_seconds(s):
    if s is None:
        return 0.0
    if not re.fullmatch(r'\d+(?:\.\d+)?|\d+:\d+(?::\d+(?:\.\d+)?)?', s):
        raise ValueError('invalid seek')
    value = sum(float(v) * 60 ** i for i, v in enumerate(reversed(s.split(':'))))
    if not math.isfinite(value) or value < 0:
        raise ValueError('invalid seek')
    return value


def probe(source):
    r = subprocess.run([PROBE, '-v', 'error', '-show_entries',
                        'stream=index,codec_type,codec_name,width,height,r_frame_rate',
                        '-of', 'json', source], capture_output=True, text=True,
                       timeout=15, check=True)
    return {int(s['index']): s for s in json.loads(r.stdout)['streams']}


def main():
    if '-hls_segment_type' not in args:
        return legacy()
    try:
        o = parse()
        if o.get('-hls_segment_type') != 'fmp4':
            return legacy()
        if os.environ.get('CEDAR_JELLYFIN_ENABLE') != '1':
            return fallback('not enabled')
        if o.get('-codec:v:0') != 'libx264' or o.get('-codec:a:0') != 'copy':
            return fallback('codec mismatch')
        if o.get('-f') != 'hls' or o.get('-hls_time') != '3':
            return fallback('hls mismatch')
        if o.get('-hls_playlist_type') != 'vod' or o.get('-hls_list_size') != '0':
            return fallback('playlist mismatch')
        if o.get('-start_number', '0').isdigit() is False:
            return fallback('invalid start number')
        if not o.get('-hls_fmp4_init_filename') or not o.get('-hls_segment_filename'):
            return fallback('missing hls paths')
        if o.get('-hls_segment_options', 'movflags=+frag_discont+skip_sidx') != 'movflags=+frag_discont+skip_sidx':
            return fallback('unsupported segment options')
        if o.get('-map_metadata', '-1') != '-1' or o.get('-map_chapters', '-1') != '-1':
            return fallback('metadata mismatch')
        if o.get('-avoid_negative_ts', 'disabled') != 'disabled':
            return fallback('timestamp mismatch')
        if o.get('-force_key_frames:0', 'expr:gte(t,n_forced*3)') != 'expr:gte(t,n_forced*3)':
            return fallback('keyframe mismatch')
        if o.get('-sc_threshold:v:0', '0') != '0':
            return fallback('scene cut mismatch')
        if o.get('-profile:v:0', 'high') != 'high' or o.get('-level', '40') != '40':
            return fallback('profile mismatch')
        if o.get('-preset', 'veryfast') != 'veryfast' or o.get('-crf', '23') != '23':
            return fallback('encoder settings mismatch')
        if o.get('-x264opts:0', 'subme=0:me_range=16:rc_lookahead=10:me=hex:open_gop=0') != 'subme=0:me_range=16:rc_lookahead=10:me=hex:open_gop=0':
            return fallback('x264 settings mismatch')
        if o.get('-copyts', False) is not True:
            return fallback('copyts not requested')
        # Cedar does not reproduce high profile, libx264 CRF, VBV, or FFmpeg color filters.
        # Require explicit opt-in for this known experimental deviation.
        if os.environ.get('CEDAR_V3_ALLOW_DEVIATIONS') != '1':
            return fallback('experimental deviations not enabled')
        maps = [m for m in o['maps'] if m != '-0:s']
        if len(maps) != 2 or not all(re.fullmatch(r'0:\d+', m) for m in maps):
            return fallback('unsupported maps')
        video_index, audio_index = [int(m.split(':')[1]) for m in maps]
        if video_index != 0:
            return fallback('unsupported video index')
        source = o.get('-i', '')
        if source.startswith('file:'):
            source = source[5:]
        if not source.lower().endswith('.mkv') or not os.path.isfile(source):
            return fallback('unsupported source')
        streams = probe(source)
        video, audio = streams.get(video_index, {}), streams.get(audio_index, {})
        if video.get('codec_type') != 'video' or video.get('codec_name') != 'h264':
            return fallback('video must be h264')
        if (video.get('width'), video.get('height')) != (1920, 1080):
            return fallback('only 1080p validated')
        if audio.get('codec_type') != 'audio' or audio.get('codec_name') != 'aac':
            return fallback('audio must be aac')
        # Accept numerically equivalent 23.976 fps representations.
        # The feeder continues to configure integer FPS=24.
        try:
            from fractions import Fraction
            fps = float(Fraction(video['r_frame_rate']))
        except (KeyError, TypeError, ValueError, ZeroDivisionError):
            return fallback('invalid fps')
        if abs(fps - (24000 / 1001)) > 0.001:
            return fallback('untested fps')
        vf = o.get('-vf', '')
        expected_vf = ('setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709,'
                       'scale=trunc(min(max(iw\\,ih*a)\\,1920)/2)*2:trunc(ow/a/2)*2,format=yuv420p')
        if vf and vf != expected_vf:
            return fallback('filter mismatch')
        try:
            rate = int(o['-maxrate'])
            buffer_size = int(o['-bufsize'])
        except (KeyError, ValueError):
            return fallback('missing bitrate')
        if not 100000 <= rate <= 20000000 or buffer_size != 2 * rate:
            return fallback('unsupported bitrate/buffer')
        seek = seek_seconds(o.get('-ss'))
        # Validate HLS destinations without modifying them.
        output = Path(o['output'])
        segment_path = Path(o['-hls_segment_filename'])
        if output.suffix != '.m3u8' or not output.is_absolute() or not segment_path.is_absolute():
            return fallback('invalid hls destinations')
        if output.parent != segment_path.parent:
            return fallback('hls paths differ')
        if '/' in o['-hls_fmp4_init_filename'] or '..' in o['-hls_fmp4_init_filename']:
            return fallback('unsafe init filename')
        if '%d' not in str(segment_path):
            return fallback('segment filename missing index')
        session = 'cedar-' + os.urandom(16).hex()
        cmd = [sys.executable, CONTROL, source, '-hide_banner', '-nostdin',
               '-loglevel', 'warning', '-f', 'mpegts', '-i', 'pipe:0']
        if '-ss' in o:
            cmd += ['-seek2any', '1', '-ss', str(seek)]
        cmd += ['-i', source, '-map', '0:v:0', '-map', f'1:{audio_index}',
                '-c:v', 'copy', '-c:a', 'copy', '-shortest', '-f', 'hls', '-hls_time', '3',
                '-hls_segment_type', 'fmp4',
                '-hls_fmp4_init_filename', o['-hls_fmp4_init_filename'],
                '-hls_playlist_type', 'vod', '-hls_list_size', '0',
                '-start_number', o.get('-start_number', '0'),
                '-hls_segment_filename', str(segment_path),
                '-hls_segment_options', o.get('-hls_segment_options', 'movflags=+frag_discont+skip_sidx'),
                '-y', str(output)]
        record = {'mode': 'cedar-fmp4-v3', 'session': session,
                  'source': source, 'video_stream': video_index,
                  'audio_stream': audio_index, 'bitrate_target': rate,
                  'vbv_enforced': False, 'profile_requested': 'high',
                  'profile_output': 'baseline', 'seek': seek,
                  'output': str(output), 'command': cmd}
        if os.environ.get('CEDAR_V3_DRY_RUN') == '1':
            print(json.dumps(record, ensure_ascii=False, indent=2))
            return 0
        LOGDIR.mkdir(parents=True, exist_ok=True)
        with (LOGDIR / 'adapter-fmp4-v3.jsonl').open('a') as f:
            f.write(json.dumps({k: v for k, v in record.items() if k != 'command'}) + '\n')
        env = os.environ.copy()
        env.pop('CEDAR_SUBTITLE_FILE', None)
        env.pop('CEDAR_SUBTITLE_FONTS', None)
        env.update({'CEDAR_BITRATE': str(rate), 'CEDAR_SEEK_SECONDS': str(seek),
                    'CEDAR_MAX_PACKETS': '0', 'CEDAR_TIMEOUT': '180',
                    'CEDAR_STREAM_TS': '1', 'CEDAR_SESSION_ID': session,
                    'CEDAR_LOG': str(LOGDIR / ('session-' + session + '.log'))})
        os.execve(sys.executable, cmd, env)
    except (ValueError, KeyError, OSError, subprocess.SubprocessError,
            json.JSONDecodeError) as e:
        return fallback(type(e).__name__ + ': ' + str(e))


if __name__ == '__main__':
    sys.exit(main())
