#!/usr/bin/env bash
set -euo pipefail

output=/tmp/uu-vulkan-encode-test.mp4

printf 'Wine: '
wine --version
printf 'GPU: '
lspci -nn | sed -n '/VGA\|Display\|3D controller/{p;q;}'

ffmpeg -hide_banner -loglevel warning \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' -t 2 \
  -init_hw_device vulkan=vk:0 -filter_hw_device vk \
  -vf 'format=nv12,hwupload' -c:v h264_vulkan -b:v 8M \
  -y "$output"

ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate,bit_rate \
  -of default=noprint_wrappers=1 "$output"
