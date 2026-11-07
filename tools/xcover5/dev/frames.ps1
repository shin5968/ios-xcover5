# Pull frames out of a phone video (the user films the console; logs are read from the video).
#   powershell -File tools/xcover5/dev/frames.ps1 VIDEO.mp4 OUTDIR [every-N-seconds]
# Writes OUTDIR\ov.jpg (contact sheet). For a moment: ffmpeg -ss T -i v.mp4 -frames:v 1 -vf "crop=iw:ih*0.5:0:ih*0.1,scale=1000:-1" x.jpg
# Note: copy videos with Copy-Item -LiteralPath (their names have [1] in them).
param([string]$Video, [string]$Out, [int]$Every = 4)
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item -LiteralPath $Video "$Out\v.mp4"
ffprobe -v error -show_entries format=duration -of csv=p=0 "$Out\v.mp4"
ffmpeg -v error -y -i "$Out\v.mp4" -vf "fps=1/$Every,scale=300:-1,tile=10x5" -frames:v 1 "$Out\ov.jpg"
