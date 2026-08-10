# PipeWire Audio Driver for VCV Rack

A VCV Rack 2 plugin that adds a native **PipeWire** entry to the audio driver
menu on Linux. It contains **no modules** — installing it changes what the
stock VCV Audio 2/8/16 modules can talk to, nothing else.

## Why

Rack's Linux audio backends all go through RtAudio, and none of them gives you
a first-class PipeWire node:

- **ALSA** and **PulseAudio** flatten the graph away.
- **JACK** works (on most distros `libjack.so.0` *is* pipewire-jack), but
  RtAudio's JACK backend invents "devices" out of the physical clients instead
  of handing Rack a node you can route freely.

This driver creates a real `pw_filter` node with named DSP ports, and either
leaves it for you to wire up or links it to the card you picked.

## Devices

| Device | What it does |
|---|---|
| `Manual routing (2 ch)` | Bare node, left unconnected. Wire it in qpwgraph / Helvum / `pw-link`. |
| `Manual routing (16 ch)` | Same, with 16 channels each way. |
| *one entry per sink* | Every audio sink in the graph — sound cards and virtual sinks. Selecting one links our ports to it automatically. |

For a sink that belongs to a real card, the capture side is paired via
`device.id`, so picking a card gives a duplex device with the hardware's real
channel counts (e.g. a Bitwig Connect Pro shows up as 6 in / 12 out).

Rack lists one menu entry per block of channels the module can handle, so a
16-channel device appears eight times in an Audio-2's menu, once per pair.
That is Rack's own behaviour, not this driver's.

## Building

Needs the Rack SDK and the PipeWire development headers.

```sh
pacman -S pipewire            # or: apt install libpipewire-0.3-dev
make RACK_DIR=/path/to/Rack-SDK
make RACK_DIR=/path/to/Rack-SDK dist
```

`libpipewire` is opened with `dlopen()` and never linked. On Windows, macOS, or
a Linux box with no PipeWire, the plugin loads normally and simply registers no
driver. The headers are only needed on the build host; without them the plugin
still compiles, it just does nothing.

## Installing

Copy `dist/*.vcvplugin` into your Rack user plugins directory
(`~/.local/share/Rack2/plugins-lin-x64/`) and restart Rack.

## Known limitations

- **Sample rate.** PipeWire ignores a rate that isn't listed in
  `clock.allowed-rates`. The driver reports the rate the graph actually granted
  rather than the one Rack asked for, and logs a warning telling you so. Fix it
  in `~/.config/pipewire/pipewire.conf.d/`, not here.
- **No live re-negotiation.** If the graph changes rate mid-session, audio keeps
  flowing but Rack's engine sample rate goes stale. Reopen the device.
- **Device list is a snapshot.** Sinks that appear or disappear while the menu
  is open aren't reflected until it is reopened.

## Licence

GPL-3.0-or-later.
