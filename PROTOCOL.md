# Satellite protocol

How a satellite and the Voice Incubator app talk. The satellite is the
microphone, the speaker and whatever shows what is happening (a ring, a
screen); the app does everything else: it decides when a command ended,
recognizes it, carries it out, answers, and keeps the satellite's wake words.

This is protocol **2**. The Voice PE firmware in this repository speaks it
(`components/voice_incubator`); nothing here is specific to the VPE except
where a section says so. [Devices](#devices) says what a satellite other than
the VPE -- an ESP32-S3-BOX-3, a Linux machine -- would declare.

## Connection

- The satellite listens on TCP (port 6055 by default) and serves **one app at
  a time**: it refuses a second connection while one is open.
- It advertises itself over mDNS as `_voice-incubator._tcp`, with TXT records
  `mac` (its MAC address) and `protocol` (`2`). For apps from before the rename,
  it may also advertise `_voice-labs._tcp`; the app treats both as one
  satellite, keyed by `mac`.
- The app connects, and the satellite says `HELLO` first.
- Either side may close the connection at any time; the app reconnects. A
  satellite drops a call in progress when the app goes away. The app clears
  its wake word models and pushes them again on every connection.
- TCP keepalive is recommended on both sides: a satellite that loses power
  sends no FIN.

## Frames

Every message is one frame:

| Bytes | |
|---|---|
| 1 | message type |
| 2 | payload length, big-endian, at most 4096 |
| n | payload |

Audio payloads are raw PCM: **16 kHz, 16-bit signed little-endian, mono**, in
both directions. Every other payload is a JSON object (UTF-8). An unknown
message type is logged and ignored; unknown JSON fields are ignored.

| Type | Name | From | Payload |
|---|---|---|---|
| `0x01` | `HELLO` | satellite | what it is and can do |
| `0x02` | `CALL_START` | satellite | a call began |
| `0x03` | `AUDIO_IN` | satellite | the call's audio |
| `0x04` | `CALL_END` | satellite | the satellite ended the call |
| `0x05` | `STATUS` | satellite | playback buffer, dropped audio |
| `0x06` | `PLAYBACK_DRAINED` | satellite | everything sent has played |
| `0x07` | `MODEL_RESULT` | satellite | a pushed model loaded, or didn't |
| `0x08` | `AUDIO_IN_AEC` | satellite | the call's second capture |
| `0x09` | `DEVICE_EVENT` | satellite | something happened on it |
| `0x0A` | `AUDIO_MONITOR` | satellite | the microphone outside a call |
| `0x0B` | `WAKE_REPORT` | satellite | what its wake word models hear |
| `0x0C` | `AUDIO_MONITOR_AEC` | satellite | the second capture outside a call |
| `0x10` | `AUDIO_OUT` | app | audio to play |
| `0x11` | `AUDIO_OUT_END` | app | the end of what is being played |
| `0x12` | `EVENT` | app | what is happening, to show |
| `0x13` | `HANGUP` | app | end the call |
| `0x14` | `MONITOR` | app | stream outside calls, or stop |
| `0x20` | `MODEL_START` | app | a wake word model's header |
| `0x21` | `MODEL_DATA` | app | part of its blob |
| `0x22` | `MODEL_END` | app | the end of its blob |
| `0x23` | `MODEL_CLEAR` | app | drop every model |

## HELLO

What the satellite is and can do. Absent fields are false or unknown.

| Field | |
|---|---|
| `protocol` | `2` |
| `name` | its node name: unique on the network, and how Home Assistant knows it (an ESPHome device's name) |
| `mac` | its MAC address: what the app keys the satellite on, so it survives renaming and reflashing |
| `model` | what it is: `voice-pe`, `box-3`, `linux`, ... |
| `firmware` | its firmware's version, shown in the app |
| `esphome` | the ESPHome version it was built with, if any |
| `sample_rate`, `channels`, `bits_per_sample` | `16000`, `1`, `16`: the only format |
| `preroll_ms` | how much audio from before the wake word a call starts with |
| `output_buffer_bytes` | its playback buffer |
| `aec` | it sends a second capture of each call, `AUDIO_IN_AEC` |
| `aec_lead_samples` | how far the second capture runs ahead of the first (absent: the VPE's, 905) |
| `monitor_aec` | it sends the second capture while monitored too, `AUDIO_MONITOR_AEC` |
| `wake_format` | the wake word models it runs, pushed by the app: `mixednet`. Absent: none from the app |
| `model_slots` | how many models it holds at once |
| `wake_models` | how many it holds now |
| `feature_size`, `max_pool` | what its `mixednet` executor supports |
| `stop_word` | it keeps "stop" loaded and runs it only while armed (a reply, a ringing timer) |
| `listen` | it opens the microphone again when a `HANGUP` asks for an answer |
| `monitor` | it can stream outside calls (`MONITOR`) |
| `intercom` | it can be joined to another satellite (`MONITOR` with `intercom`) |

**Protocol 1** (the Voice Labs firmware) had no `model`, `firmware`,
`esphome`, `aec` or `wake_format`: a protocol 1 satellite is a VPE, with both
captures and `mixednet` wake words.

## Calls

A call is one turn: the satellite heard its wake word (or its button was
pressed), streams what follows, and plays the answer.

1. **`CALL_START`**, then the pre-roll and the live audio as `AUDIO_IN` frames
   (and, with `aec`, the same for the second capture as `AUDIO_IN_AEC`,
   interleaved). Fields:

   | Field | |
   |---|---|
   | `wake_word` | the `wake_word` the model that fired was pushed with (the app pushes its id); or `manual` (its button: press to talk), `follow_up` (the answer the app asked for), `stop` (said outside a call: stop a ringing timer) |
   | `preroll_bytes` | how many bytes of `AUDIO_IN` come from before the detection |
   | `aec_preroll_bytes` | the same for `AUDIO_IN_AEC` |
   | `probability`, `max_probability`, `cutoff` | what the detection came in with, 0-255: its window's mean, the highest single inference, the threshold |

   The pre-roll ends at the detection, so the app knows where the wake word
   ended in each capture. `follow_up` has none: `0`.

2. The app decides when the command ended, and sends `EVENT`s as it goes
   (`listening`, `processing`, `recognized`, `responding`, ...; see
   [Events](#events)). The satellite keeps streaming until it is hung up on.

3. The app plays its answer: `AUDIO_OUT` frames, then `AUDIO_OUT_END`. When
   everything sent has played, the satellite says `PLAYBACK_DRAINED`.

4. The app sends **`HANGUP`** `{"reason": ..., "listen": true}`. The satellite
   stops streaming. With `listen` (only to a satellite that said `listen` in
   `HELLO`), it starts the call that hears the answer at once: `CALL_START`
   with `wake_word: follow_up` and no pre-roll -- unless it is muted.

The satellite ends a call itself with **`CALL_END`** `{"reason": ...}`:
`timeout` (no `HANGUP` in time; the VPE waits 30 s), or `cancelled` (its button,
during the call). After `cancelled`, it drops `AUDIO_OUT` still arriving for
that call until the app ends the stream (`AUDIO_OUT_END`), and the app stops
answering.

A wake word heard while a call is going on is ignored, but "stop" with
`stop_word`: it ends the reply being said (the satellite does what its button
does).

## Playback

`AUDIO_OUT` is played as it arrives, in calls and outside them (a ringing
timer, an announcement, the intercom). The app paces it by TCP backpressure:
the satellite reads the socket no faster than it can buffer. `AUDIO_OUT_END`
ends a stream; `PLAYBACK_DRAINED` follows once it has played.

`STATUS`, sent every 200 ms or so during a call, is for diagnosis: `free_bytes`,
`queued_bytes`, `queued_ms`, `speaker_busy`, `mic_dropped_bytes`,
`aec_dropped_bytes`.

## Events

`EVENT` `{"type": ..., "data": {...}}` tells the satellite what is happening,
for whatever it shows it on: the VPE's ring, a screen. A satellite shows what
it can and ignores the rest.

| `type` | When | `data` |
|---|---|---|
| `listening` | the command is being heard: after a wake word, once the app has verified it (the VPE's ring stands still until then); after `follow_up`, at once | `stage` |
| `processing` | the command ended; recognizing it | `speaker` (a name, or `unknown`), `score`, and their color `r`, `g`, `b` |
| `recognized` | it was understood | `text`, `command` (its id; `answer`, `question`) |
| `not_recognized` | it wasn't | `heard` |
| `responding` | the answer is being said | `text` |
| `result` | how it went, as the call ends | `ok` |
| `wake_rejected` | the wake word wasn't one, on second thought: no call | `reason` |
| `wake_overturned` | a wake word that was doubted is answered after all | `reason` |
| `timer_progress` | its area's running timers changed (and now and then) | `shown` (0: none), `remaining`, `total`, `paused`; `count`, `scale`, and for each, soonest first, `left<i>` and `color<i>` |
| `timer_ringing` | a timer finished here | `timers` (how many), `name` |
| `timer_stopped` | it was stopped | |
| `announcement` | an announcement starts and ends | `active` |
| `night` | night mode, on or off: light and sound kept to a minimum | `on` |

## Device events

`DEVICE_EVENT` `{"type": ...}` tells the app something happened on the
satellite.

| `type` | |
|---|---|
| `announcing`, `announced` | Home Assistant's announcement started and finished playing on its media player (speech from a Home Assistant TTS entity) |
| `stop_timer` | its button, while a timer rang |
| `intercom_end` | its button, during the intercom: the app ends it on the other satellite too |

## Wake word models

A satellite with a `wake_format` runs the models the app pushes; none is built
in. After `HELLO`, the app sends `MODEL_CLEAR` and then each model: a
`MODEL_START` header, its blob in `MODEL_DATA` frames, and `MODEL_END`
`{"id": ...}`. The satellite answers each with `MODEL_RESULT`
`{"ok", "id", "models", "error"}`. Never during a call: clearing stops the
microphone.

`MODEL_START`:

| Field | |
|---|---|
| `format` | `mixednet` (a satellite refuses another: `MODEL_RESULT` `ok: false`) |
| `slot` | where it goes |
| `id`, `wake_word` | its id (what `CALL_START` says) and name |
| `kind` | `wake`, or `vad` |
| `size`, `sha256` | of the blob |
| `probability_cutoff` | 0-1: what its window's mean must reach |
| `sliding_window_size` | inferences averaged |
| `feature_step_size` | ms between feature frames; must match the satellite's |

`mixednet` is microWakeWord's model lowered by the app to a flat table of ops
and weights, run by the satellite's executor on its own spectrogram features
(ESPHome's micro speech frontend, 40 features).

With `stop_word`, "stop" is pushed with the wake words and stays: the satellite
runs it only while it arms it (a reply, a ringing timer).

## Monitoring and the intercom

`MONITOR` `{"active": true, "intercom": false}`: stream the microphone outside
calls -- someone is listening on the app's Test page. The satellite sends
`AUDIO_MONITOR` (and, with `monitor_aec`, `AUDIO_MONITOR_AEC`), and about ten
times a second a `WAKE_REPORT`: `{"models": [{"id", "vad", "cutoff", "peak",
"window"}], "detected": [ids]}`, each model's highest probability since the
last report (0-255), of an inference and of its window's mean. Wake words
detected meanwhile are reported, not answered. It should show that it is
listened to (the VPE's ring blinks red).

With `intercom: true`, another satellite hears this one: the satellite streams
the first capture only and plays what arrives as `AUDIO_OUT`. Its button ends
it (`DEVICE_EVENT` `intercom_end`).

`MONITOR` `{"active": false}` stops it.

## Devices

| | Voice PE | ESP32-S3-BOX-3 | Linux |
|---|---|---|---|
| `model` | `voice-pe` | `box-3` | `linux` |
| Captures | XMOS: AGC tap (`AUDIO_IN`), AEC tap (`AUDIO_IN_AEC`, `aec_lead_samples` 905) | one, after the ESP32-S3's audio front end; `aec` only with a second tap | one; `aec` only with a second capture |
| `wake_format` | `mixednet` | `mixednet` (same ESP32-S3 executor) | none until it runs `mixednet`; press to talk, or wake words of its own (`CALL_START` with their name) |
| Shows events on | the LED ring | its screen (`text` of `recognized` and `responding`, timers) | LEDs, if any |
| Button | center button: press to talk, stop, cancel, end the intercom | its buttons or screen | optional |
| Status | built | not yet | not yet |

A satellite need not implement everything: the minimum is `HELLO`, calls
(`CALL_START`, `AUDIO_IN`, `CALL_END`), `HANGUP`, and playback (`AUDIO_OUT`,
`AUDIO_OUT_END`, `PLAYBACK_DRAINED`). Everything else is said in `HELLO` or
ignored when it isn't understood.
