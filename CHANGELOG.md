# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/); versions match
`plugin.json`.

## [2.0.0] — 2026-08-11

First public release. Built against the Rack SDK 2.6.6.

### Added
- Native **PipeWire** audio driver for VCV Rack 2 on Linux. It registers itself
  with `audio::addDriver()` from `init()`, so it shows up in the driver dropdown
  of every module that owns an audio port (VCV Audio 2/8/16 and equivalents).
  The plugin contains no modules.
- Creates a real `pw_filter` node with named DSP ports (`in_N` / `out_N`)
  instead of going through the RtAudio layer.
- Two kinds of device. **Manual routing (2/16 ch)**: the node is left
  unconnected for you to wire up in qpwgraph, Helvum or `pw-link`. **One entry
  per sink in the graph** (sound cards and virtual sinks): opening it links our
  ports to that sink automatically. The capture side of the same card is paired
  via `device.id`, so picking a card yields a duplex device with the hardware's
  real channel counts.
- `libpipewire` is loaded with `dlopen()` and never linked: on Windows, macOS or
  a Linux box without PipeWire the plugin loads normally and registers no
  driver. The headers are only needed on the build host. If there is no PipeWire
  server to connect to, the driver is not registered either.
- A second, long-lived connection mirrors the PipeWire registry; that is where
  the sink list and the port IDs needed by the link factory come from.
- The node is declared `node.always-process` so Rack's engine keeps running even
  with nothing connected to it.
- Sample rate and quantum are read *after* the links are created, not before:
  attaching to a card can change the node's driver and with it the clock. If
  PipeWire ignores the requested sample rate (not in `clock.allowed-rates`) or
  the quantum, a WARN explains what to change.

### Known limitations
- PipeWire ignores a sample rate that isn't in `clock.allowed-rates`. The driver
  reports the rate the graph actually granted and logs a warning.
- No live re-negotiation: if the graph changes rate mid-session, audio keeps
  flowing but Rack's engine sample rate goes stale. Reopen the device.
- The device list is a snapshot; sinks appearing or disappearing while the menu
  is open aren't reflected until it is reopened.

### Notes
- Extracted from the `UZZ-VCV-RACK` modules repo, where it was prototyped. The
  `driverId` (`0x555A5A01`) is kept so patches saved during development still
  resolve.
