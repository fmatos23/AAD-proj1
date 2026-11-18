#!/bin/bash
#
# AAD 2026/2027, assignment A1 --- the "awesome" item: a short movie whose PPM frames all have SHA1
# hashes that end with 32 zero bits
#
# usage: ./make_movie.sh [input video] [seconds] [width]
#   input video : any video file (for example one recorded with a phone); without it (or with "-")
#                 ffmpeg makes one: Conway's Game of Life (orange live cells, blue trails of the cells that died)
#   seconds     : length of the movie (default 4, at 24 frames per second: 96 frames)
#   width       : width of the frames in pixels (default 320; the height keeps the aspect ratio)
#
# the search engine is chosen automatically (fastest first): sha1_find_opencl (GPU), sha1_find_cuda
# (NVIDIA GPU), sha1_find with all cores; force one with ENGINE="./sha1_find -e avx2 -t 0" ./make_movie.sh
#
# steps (the same ones the teacher used for examples/example_movie.mkv):
#   1. ffmpeg extracts the frames as PPM files                          -> movie/original/
#   2. sha1_find changes the least significant bits of the last pixels   -> movie/x/
#   3. ffmpeg builds a lossless FFV1 movie from them                     -> movie/aad_movie.mkv
#   4. the frames are extracted again from the movie and compared        -> movie/y/
#   5. every extracted frame is checked (SHA1 with 32 zero bits, valid PPM)
#
set -e
export LC_ALL=C
cd "$(dirname "$0")"
INPUT=${1:--}
SECS=${2:-4}
WIDTH=${3:-320}
FPS=24
OUT=movie
BITS=${ZBITS:-32} # (ZBITS=20 ./make_movie.sh makes a quick test of the whole pipeline)

command -v ffmpeg > /dev/null || { echo "ffmpeg is missing (sudo apt install ffmpeg)"; exit 1; }

# search engine
if [ -z "$ENGINE" ]; then
  if make -s sha1_find_opencl 2> /dev/null && ./sha1_find_opencl -L 2> /dev/null | grep -q "device 0"; then
    ENGINE="./sha1_find_opencl -e opencl"
  elif command -v nvcc > /dev/null && make -s sha1_find_cuda 2> /dev/null; then
    ENGINE="./sha1_find_cuda -e cuda"
  else
    make -s sha1_find
    BEST=$(./sha1_find 2>&1 | sed -n 's/^engines: //p' | tr ' ' '\n' | grep -v -x -e sha_ni -e neon_sha1 | tail -1)
    ENGINE="./sha1_find -e $BEST -t 0"
  fi
fi
echo "search engine: $ENGINE"

# 1. frames
rm -rf "$OUT/original" "$OUT/x" "$OUT/y"
mkdir -p "$OUT/original" "$OUT/x" "$OUT/y"
SCALE="scale=${WIDTH}:-2:flags=lanczos,format=rgb24"
if [ "$INPUT" = "-" ]; then
  ffmpeg -hide_banner -loglevel error -f lavfi \
         -i "life=s=$((WIDTH / 2))x$((WIDTH * 3 / 8)):rate=$FPS:ratio=0.12:mold=8:life_color=#eb6834:death_color=#2a78d6:mold_color=#1a1a19" \
         -t "$SECS" -vf "scale=$WIDTH:-2:flags=neighbor,format=rgb24" "$OUT/original/frame_%03d.ppm"
else
  ffmpeg -hide_banner -loglevel error -i "$INPUT" -t "$SECS" -r "$FPS" -vf "$SCALE" "$OUT/original/frame_%03d.ppm"
fi
N=$(ls "$OUT/original" | wc -l)
echo "$N frames of $(head -c 20 "$OUT/original/frame_001.ppm" | sed -n 2p | tr -d '\n') pixels"

# 2. the search, frame by frame
T0=$(date +%s.%N)
for f in "$OUT"/original/*.ppm; do
  name=$(basename "$f")
  $ENGINE -z "$BITS" "$f" "$OUT/x/$name" | grep -E "^sha1:" | sed "s|^|$name  |"
  [ -f "$OUT/x/$name" ] || { echo "ERROR: no solution for $name"; exit 1; }
done
T1=$(date +%s.%N)
echo "search time: $(awk -v a="$T0" -v b="$T1" 'BEGIN {printf "%.1f", b - a}') s for $N frames"

# 3. lossless movie (the teacher's command)
rm -f "$OUT/aad_movie.mkv"
ffmpeg -hide_banner -loglevel error -framerate $FPS -pattern_type glob -i "$OUT/x/*.ppm" -vcodec ffv1 -level 3 -threads 1 \
       -coder 1 -context 1 -g 4 -slices 24 -slicecrc 1 "$OUT/aad_movie.mkv"
echo "movie: $OUT/aad_movie.mkv ($(du -h "$OUT/aad_movie.mkv" | cut -f1))"

# 4. extract the frames again and compare them with the ones that were written
ffmpeg -hide_banner -loglevel error -i "$OUT/aad_movie.mkv" "$OUT/y/frame_%03d.ppm"
if diff -rq "$OUT/x" "$OUT/y" > /dev/null; then
  echo "the $N frames extracted from the movie are identical to the modified frames"
else
  echo "ERROR: the frames extracted from the movie differ:"; diff -rq "$OUT/x" "$OUT/y" | head; exit 1
fi

# 5. independent check
python3 verify_files.py -z "$BITS" "$OUT"/y/*.ppm > "$OUT/verify.txt" && echo "all $N frames: SHA1 ends with $BITS zero bits and valid PPM (see $OUT/verify.txt)" \
  || { echo "ERROR: some frames failed the check:"; grep FAIL "$OUT/verify.txt"; exit 1; }

# a playable copy (MP4, lossy: only for showing, its frames do NOT keep the hashes)
ffmpeg -hide_banner -loglevel error -y -i "$OUT/aad_movie.mkv" -vf "scale=iw*2:ih*2:flags=neighbor" -pix_fmt yuv420p "$OUT/aad_movie_preview.mp4" \
  && echo "preview for slides/phones: $OUT/aad_movie_preview.mp4 (lossy, the hashes are only in the .mkv)"
