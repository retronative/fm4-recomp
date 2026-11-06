# Audio: render client and output

Goal: a real audible backend for initial validation; then a mute mode that keeps decoding, timing and callbacks;
unsupported operations explicit.

## Implementation
- runtime/kernel/audio.{h,cpp}: one XAudio render client (Xenia audio_system.cc): 64 permits at registration, one guest
  callback per permit on a guest service thread, one permit back per consumed frame, silence (no permit) when empty.
  Explicit stops: no sink, a second client, unknown driver, >256 queued frames. Unregister/ducker/change-mask: generated strict stops.
- runtime/audio_sdl.cpp: SDL3 default playback device, 48 kHz float 6 ch input (SDL converts to the device).
- mute: no device; a 48 kHz clock thread pulls frames through the same ConsumeFrame (conversion, pacing, callbacks).
- fm4recomp --audio audible|mute (required); run_attempt.sh / gpu_run.sh take <audio> (required) and check the log line.
- XAudioGetVoiceCategoryVolume: 1.0f (Xenia stub value; policy: no system mixer).
- tests/runtime/test_runtime.cpp: frame layout (hand-derived IEEE bits), INVALIDARG, no-sink stop, 64 callbacks,
  consume -> callback, silence -> none, explicit stops. 0 failures.

## First runs (each 30 s deadline, audible, stop-at-swap 100000)
- First: default device was a Bluetooth headset (coreaudio, 48000 Hz, 6 ch, 1024-frame buffer); stop 3 XAudioGetVoiceCategoryVolume.
- Second: stop 5 at 7FEA1804.
- Third/fourth: registered callback 82C56FA0, arg 3E034480; "first callback entered"; stop 5 at 7FEA1804
  (XMA register 0x601: Xenia xma_decoder.cc:344 "written with 0x02000000 and 0x03000000 around a lock operation").

## Status of the verification items at that point
- playback of title audio: UNRESOLVED (the mixer stops in its first callback, at XMA).
- buffer completion and callback release: VERIFIED in tests only (synthetic callback); not yet with the title.
- A/V sync: UNRESOLVED (no frame consumed from the title yet; the report compares output time, wall time and vblanks/60).
- The default device is a Bluetooth headset: OBSERVED (why mute must open no device).

## Next
XMA decoder: Homebrew FFmpeg 8 (libavcodec 63) has `xma2` and `xma1` decoders (Xenia's approach).

## XMA (FFmpeg for XMA)
- FFmpeg 7.1.1 (tarball SHA-256 733984395e…a03b1, same as a reference port's independent pin; gpg signature not checked:
  no gpg) + patches/ffmpeg/0001 (xmaframes decoder ported from xenia-canary/FFmpeg_radixsplit); static,
  configure: "LGPL version 2.1 or later", CONFIG_GPL/VERSION3/NONFREE 0, decoders wmapro + xmaframes only
  (tools/ffmpeg-xma/build-info.txt). Homebrew ffmpeg is GPL-3 (--enable-gpl --enable-version3): used only as a
  separate reference tool, never linked.
- Variant: disc music (e.g. Media/audio/Music/*.xma) is XMA2 (fmt tag 0x166), 44.1 kHz stereo, one stream; in-game
  contexts: packet metadata 1 (XMA2, Xenia xma_context.cc:940), 44.1 kHz mono and 48 kHz stereo.
- Register 606 reader: sub_8238FAD0 (ppc/ppc_recomp.11.cpp): lwbrx 7FEA1818, xor 0x200, waits while equal to a
  context id (u16 at entry+80): bit 9 = processing. VERIFIED by reading our translation.
- Port bug found by the real run (exit 139 in ff_decode_preinit): codec descriptors must be in id order
  (binary search); fixed in the patch. Regression: tests/xma/run_crosscheck.sh exercises avcodec_open2 for real.
- Cross-check: 400 stereo frames bit-identical to FFmpeg's xma2 decoder after a 64-sample-per-channel start offset
  (xma2 trims its start; xmaframes outputs nothing for the first frame).

## Audio validation: PASSED (OBSERVED, one audible and one mute 90 s run)
- Audible (Bluetooth headset): no stop for 90 s (deadline); 4,189 swaps, 592,164 draws (headless).
  Exact silence 0-6 s, then continuous game audio from ~7-8 s: per 2 s window peak about -10 dBFS, RMS -18..-61 dBFS,
  all 375 frames audible in most windows. Callbacks = submitted (16,878), 0 silent frames (no underrun), 64 frames in
  flight; output 89.675 s vs wall 89.670 s vs vblanks/60 89.683 s; SDL device queue bounded 5-59 ms.
- Mute (no device opened): callbacks 16,879, XMA 1,580 frames, output 89.680 / wall 89.676 /
  vblanks/60 89.683 s, per-window levels equal to the audible run's (e.g. 48 s: RMS -61.2 vs -61.1 dBFS): muting keeps
  decoding, pacing and callbacks.
- Not shown: lip-sync (needs rendered video: UNRESOLVED).
- From here, routine runs use mute; audible only for named validation runs.

## Listening check: CONFIRMED by ear
- Audible, 90 s, no stop: studio audio heard. Meter: exact silence 0-6 s, audio from ~7 s (peak -7.5 to -25 dBFS),
  continuous to the deadline.
