#!/usr/bin/env bash
# Offline fixture recipe for the 1440x1080 MJPEG source. Run from the ZenithTerminal repository root.
# Refuses to overwrite existing outputs; no encoder runs during replay.
set -euo pipefail
input=${1:?Usage: prepare-demo.sh SOURCE.avi [OUTPUT_DIRECTORY]}
out=${2:-assets/video}
mkdir -p "$out"
ffmpeg -nostdin -n -hide_banner -xerror -filter_threads 2 -threads 2 \
  -i "$input" -map 0:v:0 -an -t 20 \
  -vf 'scale=iw:ih:in_range=pc:out_range=tv:in_color_matrix=bt601:out_color_matrix=bt709,format=yuv420p,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black,setsar=1,fps=60' \
  -c:v libx265 -preset fast -tune zerolatency -profile:v main \
  -b:v 8M -maxrate 8M -bufsize 8M \
  -x265-params 'pools=4:frame-threads=2:keyint=60:min-keyint=60:scenecut=0:open-gop=0:bframes=0:rc-lookahead=0:repeat-headers=1:aud=1' \
  -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
  -movflags +faststart "$out/demo-1080p60.mp4"
ffmpeg -nostdin -n -v error -i "$out/demo-1080p60.mp4" -map 0:v:0 \
  -c:v copy -bsf:v hevc_mp4toannexb -f hevc "$out/demo-1080p60.h265"
