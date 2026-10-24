# VoiceForge

Open-source live voice changer for iPhone calls (jailbroken, roothide or rootless, iOS 15+).
Written from scratch; it shares no code with VoiceChanger XS.

## Voices

| Voice | Pitch | Formant | Notes |
|---|---|---|---|
| Normal (Off) | 1.00 | 1.00 | pass-through |
| Female | 1.70 | 1.18 | raises pitch ~9 semitones, shortens the vocal tract |
| Deep Narrator | 0.82 | 0.90 | ~3.5 semitones down, +5 dB warmth at 180 Hz |
| Dark Lord | 0.62 | 0.75 | monster voice |
| Tenor | 0.80 | 0.80 | classic pitch-down |
| Smurf | 1.60 | 1.60 | classic pitch-up |
| Chipmunk | 2.20 | 2.20 | classic pitch-up |
| Custom | slider | slider | 0.5 – 2.5 each |

*Pitch* is the note you speak at. *Formant* is the resonance of the vocal tract, which is
what makes a voice sound like a different-sized person. The engine moves them independently,
so Female sounds like a woman's voice rather than a sped-up man's.

Listen to every preset applied to a sample voice in [`samples/`](samples/).

## Build

There's no Mac needed. Push this folder to a GitHub repository, and the **Build** workflow
(Actions tab) produces two packages as downloadable artifacts:

- `VoiceForge-roothide` — install this one on roothide (Dopamine roothide).
- `VoiceForge-rootless` — for rootless jailbreaks, or convert it with RootHide Patcher if the roothide build fails.

With Theos installed locally: `make package FINALPACKAGE=1` (roothide by default,
`THEOS_PACKAGE_SCHEME=rootless` for rootless).

## Install & use

1. **Uninstall VoiceChanger XS first** (both hook the call audio; the package conflicts with it).
2. Install the `.deb` in Sileo/Zebra, then respring. Installing restarts `audiomxd` automatically.
3. Settings → VoiceForge → pick a **Voice**. It applies to your microphone while a call is active
   and can be changed mid-call.

## Finding the call audio path (first-time setup)

Apple doesn't document which audio unit carries the uplink (your voice) during a cellular call,
and it differs between iOS versions. VoiceForge hooks the public AudioToolbox entry points and
defaults to the microphone side of the voice-processing I/O unit (`render:vpio:1`). If the voice
doesn't change on a call:

1. Settings → VoiceForge → turn on **Discovery logging**, pick any voice other than Normal.
2. Stream the device log while making a short call and saying a few words:
   - Windows/macOS/Linux with [libimobiledevice](https://libimobiledevice.org): `idevicesyslog -m VoiceForge`
   - on the device (Sileo package `oslog`): `oslog | grep VoiceForge`
3. You'll see lines like
   `VoiceForge: seen fn=process unit=0x… comp=aufx/vp4 /appl bus=0 frames=480 fmt=lpcm 24000Hz ch=1 … inCall=1`.
   Lines with `inCall=1` and a mono 8–48 kHz format are the candidates.
4. Put the candidate into **Target** as `fn:subtype:bus`, e.g. `process:vp4 :0` (the subtype is
   always 4 characters; keep trailing spaces), and test again. `MATCH` in the log shows what is being processed.

The **Process outside calls** switch applies the voice without a call, for quick tests.

## Layout

- `VFDSP.c/.h` — portable C engine: peak-locked phase-vocoder pitch shifter with cepstral
  formant control, loudness matching, low-shelf and soft limiter. No allocation while processing.
- `Tweak.x` — SpringBoard side (settings + CallKit call state → `notify(3)` state word) and
  audio-server side (AudioToolbox hooks, format conversion, per-unit engines).
- `layout/Library/PreferenceLoader/Preferences/VoiceForge.plist` — the Settings pane.
- `test/vftest.c` — desktop test: `cc -O2 -o vftest VFDSP.c test/vftest.c -lm && ./vftest in.wav outdir`.
