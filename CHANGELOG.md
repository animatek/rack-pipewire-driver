# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/); las
versiones corresponden a `plugin.json`.

## [Unreleased]

### Added
- Driver de audio **PipeWire** nativo para VCV Rack 2 en Linux. Se registra con
  `audio::addDriver()` desde `init()`, así que aparece en el desplegable de
  driver de cualquier módulo con puerto de audio (VCV Audio 2/8/16 y
  equivalentes). El plugin no contiene módulos.
- Crea un nodo `pw_filter` real con puertos DSP nombrados (`in_N` / `out_N`) en
  vez de pasar por la capa RtAudio.
- Dos clases de dispositivo. **Manual routing (2/16 ch)**: el nodo se deja sin
  conectar para cablearlo en qpwgraph, Helvum o `pw-link`. **Una entrada por
  cada sink del grafo** (tarjetas y sinks virtuales): al abrirlo enlaza
  automáticamente los puertos a esa tarjeta. La parte de captura de la misma
  tarjeta se empareja por `device.id`, así que elegir una tarjeta da un
  dispositivo dúplex con los canales reales del hardware.
- `libpipewire` se carga con `dlopen()` y nunca se enlaza: en Windows, macOS o
  Linux sin PipeWire el plugin carga con normalidad y no registra driver. Los
  headers solo hacen falta en la máquina de compilación. Si no hay servidor
  PipeWire al que conectarse, el driver tampoco se registra.
- Segunda conexión de larga vida que espeja el registry de PipeWire: de ahí
  salen la lista de sinks y los IDs de puerto que necesita la link-factory.
- El nodo se declara `node.always-process` para que el engine de Rack siga
  corriendo aunque no haya nada conectado al nodo.
- El sample rate y el quantum se leen *después* de crear los enlaces, no antes:
  engancharse a una tarjeta puede cambiar el driver del nodo y con él el reloj.
  Si PipeWire ignora el sample rate pedido (no está en `clock.allowed-rates`) o
  el quantum, queda un WARN explicando qué tocar.

### Notas
- Extraído del repo de módulos `UZZ-VCV-RACK`, donde nació. El `driverId`
  (`0x555A5A01`) se conserva para no romper patches guardados durante el
  desarrollo.
