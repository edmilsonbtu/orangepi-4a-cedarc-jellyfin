#!/usr/bin/env python3
"""CedarC v6.0.3 stage 1: conservative MPEG-TS integration; not production enabled."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REAL = '/usr/lib/jellyfin-ffmpeg/ffmpeg'
PROBE = '/usr/lib/jellyfin-ffmpeg/ffprobe'
CONTROL = '/opt/cedar-native/beta54/cedar-process-control-v5.4.py'
ARGS = sys.argv[1:]
VALUE = {'-analyzeduration','-probesize','-i','-map_metadata','-map_chapters',
         '-threads','-map','-codec:v:0','-codec:a:0','-preset','-crf',
         '-maxrate','-bufsize','-profile:v:0','-level','-x264opts:0',
         '-force_key_frames:0','-sc_threshold:v:0','-vf','-avoid_negative_ts',
         '-max_muxing_queue_size','-f','-max_delay','-hls_time',
         '-hls_segment_type','-start_number','-hls_segment_filename',
         '-hls_playlist_type','-hls_list_size','-ss','-loglevel',
         '-hls_fmp4_init_filename','-hls_segment_options','-bsf:a','-bsf:v',
         '-fflags','-seek2any'}
FLAGS = {'-y','-nostdin','-hide_banner','-copyts','-start_at_zero'}

class Unsupported(Exception):
    pass

def fallback(reason):
    if os.environ.get('CEDAR_V6_DRY_RUN') == '1':
        print(json.dumps({'mode':'fallback','reason':reason},ensure_ascii=False))
        return 0
    os.execv(REAL,[REAL,*ARGS])

def parse(args):
    if not args or args[-1].startswith('-'):
        raise Unsupported('missing output')
    o={'maps':[],'output':args[-1]}
    i=0
    while i<len(args)-1:
        k=args[i]
        if k in FLAGS:
            if k in o: raise Unsupported('duplicate flag '+k)
            o[k]=True;i+=1
        elif k in VALUE:
            if i+1>=len(args)-1:raise Unsupported('missing value '+k)
            v=args[i+1]
            if k=='-map':o['maps'].append(v)
            else:
                if k in o:raise Unsupported('duplicate '+k)
                o[k]=v
            i+=2
        else:raise Unsupported('unsupported option '+k)
    return o

def probe(source):
    r=subprocess.run([PROBE,'-v','error','-show_entries',
        'stream=index,codec_type,codec_name,width,height,sample_aspect_ratio,r_frame_rate,bit_rate',
        '-of','json',source],capture_output=True,text=True,timeout=15,check=True)
    return {int(s['index']):s for s in json.loads(r.stdout)['streams']}

def prepare():
    o=parse(ARGS)
    if os.environ.get('CEDAR_JELLYFIN_ENABLE')!='1':
        raise Unsupported('cedar not enabled')
    if o.get('-hls_segment_type')!='mpegts':
        raise Unsupported('fmp4 not yet homologated; use original ffmpeg')
    if o.get('-codec:v:0')!='libx264':
        raise Unsupported('not a video transcode')
    if o.get('-codec:a:0')!='copy':
        raise Unsupported('audio codec not copy')
    if o.get('-f')!='hls' or o.get('-hls_time')!='3':
        raise Unsupported('HLS settings differ')
    if o.get('-hls_playlist_type')!='vod' or o.get('-hls_list_size')!='0':
        raise Unsupported('playlist settings differ')
    if o.get('-copyts') is not True or o.get('-start_at_zero') is not True:
        raise Unsupported('timestamp flags differ')
    if o.get('-avoid_negative_ts')!='disabled':
        raise Unsupported('negative timestamp policy differs')
    if o.get('-ss') is not None:
        raise Unsupported('seek pending A/V synchronization validation')
    if o.get('-start_number','0')!='0':
        raise Unsupported('nonzero segment start pending validation')
    maps=o['maps']
    if len(maps)!=2 or not all(re.fullmatch(r'0:\d+',m) for m in maps):
        raise Unsupported('unsupported stream maps')
    vi,ai=[int(m.split(':')[1]) for m in maps]
    if vi!=0:raise Unsupported('nonzero video stream')
    source=o.get('-i','')
    if source.startswith('file:'):source=source[5:]
    if not source.lower().endswith('.mkv') or not os.path.isfile(source):
        raise Unsupported('source not an existing mkv')
    streams=probe(source)
    v,a=streams.get(vi,{}),streams.get(ai,{})
    if v.get('codec_type')!='video' or v.get('codec_name') not in ('h264','hevc'):
        raise Unsupported('unsupported video codec')
    if a.get('codec_type')!='audio' or a.get('codec_name')!='aac':
        raise Unsupported('stage 1 requires AAC audio copy')
    width,height=v.get('width'),v.get('height')
    if not isinstance(width,int) or not isinstance(height,int) or width<320 or height<240 or width>1920 or height>1080:
        raise Unsupported('dimensions not in stage 1 range')
    # Native encoder has alignment constraints; only dimensions known to work.
    if (width,height) not in {(640,480),(854,480),(1024,576),(1280,720),(1366,768),(1408,768),(1920,1080)}:
        raise Unsupported('dimensions not validated')
    try:
        rate=int(o['-maxrate']);buf=int(o['-bufsize'])
    except (KeyError,ValueError):raise Unsupported('missing or invalid bitrate')
    if not (100000<=rate<=20000000) or buf!=rate*2:
        raise Unsupported('unsupported rate/buffer')
    vf=o.get('-vf','')
    subtitle=None;fonts=None
    if not vf:
        raise Unsupported('missing video filter; stage 1 requires exact validated chain')
    if vf:
        # Do not silently ignore transformations other than the known Jellyfin color/scale chain.
        if not vf.startswith('setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709,scale='):
            raise Unsupported('unknown video filter')
        if ',format=yuv420p' not in vf:
            raise Unsupported('unexpected format filter')
        if ',subtitles=' in vf:
            if vf.count(',subtitles=')!=1:raise Unsupported('multiple subtitle filters')
            tail=vf.split(',subtitles=',1)[1]
            m=re.fullmatch(r"f='([^']+)'(?::fontsdir='([^']+)')?",tail)
            if not m:raise Unsupported('unsupported subtitle filter')
            subtitle,fonts=m.groups()
            if not subtitle.lower().endswith(('.ass','.ssa')) or not os.path.isfile(subtitle):
                raise Unsupported('subtitle file unavailable')
            if fonts and not os.path.isdir(fonts):raise Unsupported('font directory unavailable')
        elif ',subtitles' in vf:
            raise Unsupported('unrecognized subtitle filter')
        # Exact Jellyfin filter verified for square-pixel 1920x1080 only.
        # The expression uses display aspect ratio a; non-square pixels
        # can change the output size, so fail closed for SAR != 1:1.
        filter_without_subs = vf.split(',subtitles=', 1)[0]
        expected = (
            'setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709,'
            r'scale=trunc(min(max(iw\,ih*a)\,min(4096\,2176*a))/2)*2:'
            r'trunc(min(max(iw/a\,ih)\,min(4096/a\,2176))/2)*2,'
            'format=yuv420p'
        )
        if (filter_without_subs != expected or (width, height) != (1920, 1080)
                or v.get('sample_aspect_ratio') != '1:1'):
            raise Unsupported('scale output not proven identical to input; stage 1 forbids resizing')
    output=Path(o['output']);pattern=Path(o.get('-hls_segment_filename',''))
    if (not output.is_absolute() or not pattern.is_absolute()
            or output.suffix != '.m3u8'
            or not re.fullmatch(r'[^/%]+-?%(?:0[1-9][0-9]*)?d\.ts', pattern.name)
            or output.parent != pattern.parent
            or any(part in ('.', '..') for part in output.parts + pattern.parts)
            or any(parent.is_symlink() for parent in (output.parent, *output.parent.parents))):
        raise Unsupported('unsafe HLS destinations')
    if o.get('-force_key_frames:0','expr:gte(t,n_forced*3)')!='expr:gte(t,n_forced*3)':
        raise Unsupported('unexpected keyframe policy')
    if o.get('-sc_threshold:v:0','0')!='0':raise Unsupported('scene-cut policy')
    session='cedar-'+os.urandom(16).hex()
    cmd=[sys.executable,CONTROL,source,'-hide_banner','-nostdin','-loglevel','warning',
         '-f','mpegts','-i','pipe:0','-i',source,
         '-map','0:v:0','-map',f'1:{ai}',
         '-c:v','copy','-c:a','copy','-shortest',
         '-f','hls','-hls_time','3','-hls_segment_type','mpegts',
         '-hls_playlist_type','vod','-hls_list_size','0','-start_number','0',
         '-hls_segment_filename',str(pattern),'-y',str(output)]
    env=os.environ.copy()
    env.pop('CEDAR_SUBTITLE_FILE',None);env.pop('CEDAR_SUBTITLE_FONTS',None)
    env.update({'CEDAR_BITRATE':str(rate),'CEDAR_SEEK_SECONDS':'0',
                'CEDAR_MAX_PACKETS':'0','CEDAR_TIMEOUT':'180',
                'CEDAR_STREAM_TS':'1','CEDAR_SESSION_ID':session})
    if subtitle:env['CEDAR_SUBTITLE_FILE']=subtitle
    if fonts:env['CEDAR_SUBTITLE_FONTS']=fonts
    record={'mode':'cedar-v6-stage1','source':source,'video_stream':vi,
            'audio_stream':ai,'subtitle':subtitle,'fonts':fonts,
            'bitrate':rate,'resolution':[width,height],'session':session,
            'command':cmd}
    return record,env

def main():
    try:
        record,env=prepare()
    except (Unsupported,ValueError,KeyError,OSError,subprocess.SubprocessError,json.JSONDecodeError) as exc:
        return fallback(str(exc))
    if os.environ.get('CEDAR_V6_DRY_RUN')=='1':
        print(json.dumps(record,ensure_ascii=False,indent=2));return 0
    # Explicit second opt-in: do not run unvalidated pipeline by accident.
    if os.environ.get('CEDAR_V6_EXECUTE')!='1':
        return fallback('v6 execution not explicitly enabled')
    os.execve(sys.executable,record['command'],env)

if __name__=='__main__':
    sys.exit(main())
