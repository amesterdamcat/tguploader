#!/bin/bash
DIR="${1:-.}"
COLS=10; ROWS=9; FRAMES=$((COLS*ROWS)); TILE_W=$((3840/COLS))
total=$(find "$DIR" -name "*_part2.mp4" | wc -l)
echo "[FIX-THUMBS] Found $total part2 files in $DIR"
count=0; ok=0; skip=0; fail=0
find "$DIR" -name "*_part2.mp4" -print0 | sort -z | while IFS= read -r -d '' f; do
    count=$((count+1))
    jpg="${f%.mp4}.jpg"
    fname=$(basename "$f")
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f" 2>/dev/null)
    if [ -z "$dur" ] || [ "$dur" = "N/A" ]; then
        echo "[$count/$total] SKIP (no duration): $fname"
        skip=$((skip+1)); continue
    fi
    interval=$(awk "BEGIN{printf \"%.4f\", $dur/($FRAMES+1)}")
    echo -n "[$count/$total] $fname (${dur}s) -> "
    ffmpeg -y -loglevel error -i "$f" \
        -vf "fps=1/${interval},scale=${TILE_W}:-1,drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf:text='%{pts\:hms}':fontcolor=white:fontsize=14:box=1:boxcolor=black@0.5:boxborderw=3:x=5:y=h-th-5,tile=${COLS}x${ROWS}:padding=1:color=0x333333" \
        -frames:v 1 -q:v 2 "$jpg" 2>/dev/null
    if [ $? -eq 0 ] && [ -f "$jpg" ]; then
        jsize=$(du -h "$jpg" | cut -f1)
        echo "ok ($jsize)"; ok=$((ok+1))
    else
        echo "FAILED"; fail=$((fail+1))
    fi
done
echo ""
echo "[FIX-THUMBS] Done."
