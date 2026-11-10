> [!WARNING]
> This project is experimental and not supported.

# Voice Incubator firmware

ESPHome firmware for the satellites of the
[Voice Incubator](https://github.com/OHF-Voice/app-voice-incubator) app: wake
words detected on the device with models the app pushes, and calls streamed to
the app, which recognizes and answers them.

**➡️ Install page: https://ohf-voice.github.io/firmware-voice-incubator/**

| Device | Firmware | |
|---|---|---|
| [Home Assistant Voice PE](https://www.home-assistant.io/voice-pe/) | [`voice-pe`](https://ohf-voice.github.io/firmware-voice-incubator/voice-pe/) | built |
| ESP32-S3-BOX-3 | | planned |

A satellite and the app speak [PROTOCOL.md](PROTOCOL.md): a Linux satellite,
or any other, can too.

## Layout

| | |
|---|---|
| `components/voice_incubator` | the ESPHome component: the protocol, streaming, and the wake word executor. Nothing in it is specific to one device |
| `devices/<device>.yaml` | a device's configuration: its microphones and speaker, and what shows the app's events (the VPE's ring, a screen) |
| `<device>.yaml` | the install page's build of it: Wi-Fi over improv, a name from its MAC, and updates from the install page |
| `<device>.local.yaml` | the build from a checkout: Wi-Fi from `secrets.yaml` |

The [Firmware workflow](.github/workflows/firmware.yml) builds every device on
each push and pull request; run by hand, it publishes the install page. A new
device is a `devices/<device>.yaml`, its `<device>.yaml` wrapper, and its name
in the workflow's matrix.

## From a checkout

`secrets.yaml` holds the Wi-Fi (`wifi_ssid`, `wifi_password`). Each satellite
after the first gets a name of its own:

```bash
esphome run voice-pe.local.yaml --device <ip>
esphome -s name voice-incubator-vpe-2 -s friendly_name "Voice Incubator VPE 2" run voice-pe.local.yaml
```

A satellite flashed with the Voice Labs firmware keeps its name with
`-s name` (for example `voice-labs-vpe`): Home Assistant's ESPHome integration
knows it by it. An early prototype Voice PE has 8 MB of flash, not 16: add
`-s flash_size 8MB`.

## History

This was the Voice Labs firmware, in
[OHF-Voice/voice-pe-alt-firmware](https://github.com/OHF-Voice/voice-pe-alt-firmware)
until November 2026. It speaks protocol 2 and answers to the Voice Labs app's
mDNS name too, so the app before the rename still finds it.
