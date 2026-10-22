ffplay for TSReader's embedded player (Playback > FFplay (embedded))

These files are an unmodified build of FFmpeg 9.0.2 (ffplay plus its libraries) and
SDL 2.32.10, made from the official sources with vcpkg and Visual Studio 2026:

  FFmpeg  https://github.com/FFmpeg/FFmpeg  (tag n9.0.2)
  SDL2    https://github.com/libsdl-org/SDL

  vcpkg install "ffmpeg[core,avcodec,avformat,avfilter,swresample,swscale,ffplay]:x64-windows"

FFmpeg was configured without GPL and non-free components, so it is licensed under the
GNU LGPL v2.1 or later (see LICENSE-FFmpeg.txt). SDL2 is under the zlib licence (see
LICENSE-SDL2.txt). TSReader runs ffplay.exe as a separate program and does not link to
these libraries.

ffplay is 64-bit; it runs as its own process, so it works with 32-bit TSReader.
Decoders included: H.264, HEVC, MPEG-2, AAC, AC-3/E-AC-3, MPEG audio and DVB subtitles
(no teletext subtitles).
