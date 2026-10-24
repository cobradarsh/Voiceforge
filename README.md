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

### In-call button

During a call, a pill-shaped **VoiceForge** button sits on the right of the call screen showing
the current voice ("Voice" when off; purple while a voice is active). Drag it anywhere. Tap it for
the voice menu: Off, Female, Narrator, Dark Lord, Tenor, Smurf, Chipmunk and Custom. **Custom**
opens the pitch grid (0.7 · 0.8 · 0.9 | 1.1 … 2.0) for fine-tuning; formants follow the pitch
automatically (formant = pitch^0.33). Turn the button off with Settings → VoiceForge → In-call button.

If another voice-changer tweak is installed, its own in-call button and menu are separate from
VoiceForge's; uninstall it to avoid two tweaks changing the call audio.

## Finding the call audio path (first-time setup)

**On iOS 15-18 the call mic audio runs through a chain of audio-effect units in `mediaserverd`**, not
through a voice-processing I/O unit. VoiceForge's auto target is the `vlc2` (voice-level) stage near the
end of that uplink chain. If your voice still doesn't change, open the diagnostics file, find a mono
`float32` unit while `audio-inCall=1`, and set **Target** to it, e.g. `process:vlc2:0`, `process:nnet:0`,
`multi:egt3:0`, or `process:firf:0`. The Target field applies live, with no reinstall.

VoiceForge writes `/var/mobile/Documents/VoiceForge-diagnostics.txt` (open it in Filza). It records
whether `audiomxd` loaded the tweak and could read the settings, what SpringBoard published, and
every audio unit and audio converter the audio server uses, refreshed when a call starts. Make a
short call while talking, then share that file to find the right target.

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
