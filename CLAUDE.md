# CLAUDE.md

Driver de audio PipeWire para VCV Rack.

El mapa de todos los proyectos y sus reglas de entrada está en `/mnt/SPEED/CODE/CLAUDE.md`.
Este archivo existe para que las reglas lleguen también cuando la sesión se abre **dentro** de
este repo y no desde `CODE`.

## El changelog común de CODE

Además del `CHANGELOG.md` de este repo, **todo cambio relevante se apunta también en
`/mnt/SPEED/CODE/CHANGELOG.md`**: el registro común de los siete proyectos, y lo que enseña la
página `Cambios` del panel. Sin esa línea el cambio no existe fuera de este repo — que es justo
lo que pasaba antes del 2026-09-10, cuando el panel solo leía el changelog de `Animatek.net`.

Ahí va el resumen: qué cambió, la verificación real, el agente, y el commit o la ruta del
changelog de este repo. El detalle técnico se queda aquí y no se duplica.

- Es un **enlace simbólico** a la nota de Obsidian `00 - Sistema/CHANGELOG - CODE.md`. Se edita
  el destino: nunca se sustituye por un archivo suelto ni se crea una segunda copia. Si no está
  disponible, se dice y se para; no se inventa otro sitio.
- Bajo la fecha local (Europe/Madrid), lo más reciente arriba y una sección por proyecto.
  Releer el bloque del día antes de escribir y tocar solo lo propio: ahí escriben varios agentes.
- Un cambio sin commitear se marca explícitamente como **cambio local, sin commit**.
- El panel lo recoge en la ingesta de cada mañana. Para verlo ya:
  `Animatek.net/panel/ingesta/actualizar.sh`.
- **No lo leas entero para escribir en él.** Crece unos 13 KB al día y está partido por meses:
  el mes en curso en la nota y los cerrados en `CHANGELOG - CODE/AAAA-MM.md`, al lado. Para
  consultarlo hay `cambios` — `cambios buscar "morph"`, `cambios de NME`, `cambios ver
  2026-09-10` —, que pregunta a la base y devuelve la entrada, no el archivo.
