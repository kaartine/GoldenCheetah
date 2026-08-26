#!/usr/bin/env bash

set -euo pipefail

output_dir=$(cd "$(dirname "$0")" && pwd)

# Project-authored deterministic cues. Keeping the synthesis recipe beside the
# WAV files makes their origin and future regeneration auditable.
ffmpeg -nostdin -hide_banner -loglevel error -y \
    -f lavfi -i "sine=frequency=660:duration=0.08:sample_rate=22050" \
    -f lavfi -i "sine=frequency=880:duration=0.10:sample_rate=22050" \
    -filter_complex \
    "[0:a]volume=0.16[a0];[1:a]volume=0.13,afade=t=out:st=0.05:d=0.05[a1];[a0][a1]concat=n=2:v=0:a=1[out]" \
    -map "[out]" -ac 1 -ar 22050 -c:a pcm_s16le \
    "$output_dir/workout-game-feature.wav"

ffmpeg -nostdin -hide_banner -loglevel error -y \
    -f lavfi -i "sine=frequency=90:duration=0.22:sample_rate=22050" \
    -af "volume=0.22,afade=t=out:st=0.02:d=0.20" \
    -ac 1 -ar 22050 -c:a pcm_s16le \
    "$output_dir/workout-game-landing.wav"
