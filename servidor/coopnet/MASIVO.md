# coopnet con partidas masivas (sm64coopdx-1024)

Versión modificada del servidor de lobbies de coopnet. Las partidas de **hasta 16** jugadores
funcionan exactamente igual que en el original (lobby + conexiones P2P con ICE). Las de **más de
16** (hasta 1024, públicas o privadas) son **masivas**: la red la hace smnet, y coopnet solo las
lista y ayuda a entrar.

## Qué cambia

- Crear un lobby con más de 16 jugadores lo marca como masivo (tope 1024). El servidor le manda al
  host, por la conexión TCP de siempre, un paquete nuevo `MPACKET_MASSIVE` con una clave secreta y
  los dos puertos del encuentro UDP (callback `OnMassiveLobby` en `libcoopnet.h`).
- En la lista de partidas, una masiva muestra la cantidad de jugadores que informa el host (no los
  miembros del lobby: en una masiva solo el host queda en el lobby).
- `coopnet_lobby_join` a una masiva devuelve el error nuevo `MERR_LOBBY_MASSIVE`: se entra por el
  encuentro UDP (abajo) y después con el Join masivo de smnet (que baja los mods de la partida).
- Las privadas siguen igual: solo aparecen en la lista con la contraseña correcta, y el encuentro
  también la pide.

## Encuentro UDP (`server/rendezvous.cpp`)

Dos puertos UDP: por defecto 34198 y 34199 (el TCP de coopnet es el 34197). Mensajes que empiezan
con `SMRV` y un byte de tipo (detalle en el comentario de `rendezvous.cpp`):

| Tipo | Quién → quién | Para qué |
|---|---|---|
| SONDA / SONDA_R | cualquiera → A y B | ver la dirección pública; si el puerto cambia entre A y B, el NAT es simétrico |
| REGISTRO / REGISTRO_R | host → A, cada 5 s, **desde su socket de smnet** | el servidor anota la dirección pública del host y cuántos jugadores tiene (con la clave) |
| ENTRAR / ENTRAR_R | jugador → A | contraseña; devuelve la dirección pública del host |
| PERFORAR | servidor → host | la dirección del que entra: el host le manda paquetes y abre su NAT (no hace falta abrir el puerto en el router si el NAT del host es de cono) |
| INFORME | cliente → A | su tipo de NAT, para estadísticas anónimas (cada minuto en la salida del servidor) |
| TELEMETRÍA | jugador de una masiva → A, cada 30 s | anónima: rol, NAT, FPS, jugadores, desconexiones, cambios de SN, tráfico (ver abajo) |
| VERSIÓN / VERSIÓN_R | juego → A, al arrancar | la última versión del fork, para el aviso de versión nueva |

## Correrlo

    make server lib                  # necesita libcurl (apt install libcurl4-openssl-dev)
    ./bin/server                     # TCP 34197, UDP 34198-34199
    COOPNET_PUERTO=35197 COOPNET_ENCUENTRO=35198 ./bin/server   # otros puertos

En un VPS: abrir TCP 34197 y UDP 34198-34199.

## Probar en la PC (Ubuntu en WSL o Linux)

    sh despliegue/local.sh              # compila si hace falta y lo corre (Ctrl+C lo para)
    sh despliegue/local.sh --fondo      # en segundo plano; --estado / --parar
    sh despliegue/local.sh --recompilar # de cero, después de copiar código nuevo

Es el mismo servidor del VPS. Configuración y telemetría en `~/coopnet-local/`. Muestra la dirección
que tiene que usar el juego (la IP de WSL, salvo en modo espejo). Del lado de Windows, en MSYS2,
`sh ../herramientas/jugar-local.sh 2` (desde `juego/sm64coopdx`) busca esa IP sola y abre el juego.

## Instalar en un VPS (Ubuntu o Debian)

    sudo sh despliegue/instalar.sh        # desde coopnet/, con el código

Compila, instala el binario en `/opt/coopnet`, crea el usuario `coopnet` y el servicio
`coopnet-1024` (arranca solo y se reinicia si se cae), y abre los puertos en `ufw` si está activo.
Volver a correrlo actualiza el binario sin tocar la configuración ni la telemetría.

- configuración: `/etc/coopnet-1024.env` (puertos, telemetría, última versión del fork);
  después de cambiarla, `sudo systemctl restart coopnet-1024`
- log: `journalctl -u coopnet-1024 -f`
- telemetría: `/var/lib/coopnet/telemetria.json` y `.csv`; `despliegue/nginx-telemetria.conf`
  la publica por HTTP si hay nginx
- si el proveedor tiene un firewall propio (panel web), abrir ahí también TCP 34197 y UDP 34198-34199

El juego apunta a este servidor si se compila con `make SMNET=1 COOPNET_SERVIDOR=host[:puerto]`
(o con `coopnet_ip_1024` / `coopnet_port_1024` en `sm64config.txt`, o `COOPDX_COOPNET=ip:puerto`).

## Host dedicado en el VPS

La partida la abre el mismo VPS: el juego sin pantalla (`--headless`), como un jugador invisible
que es el host y el coordinador, siempre prendido (servicio `sm64-1024-host`). Desde la carpeta del
proyecto (la que tiene `coopnet/` y `juego/sm64coopdx/`), después de `instalar.sh`:

    sudo sh coopnet/despliegue/host-dedicado.sh /ruta/a/baserom.us.z64

- compila la libcoopnet nuestra y el juego apuntando al coopnet local, instala en `/opt/sm64-1024`;
  la ROM queda en `/var/lib/sm64-1024` (solo la lee el juego)
- configuración: `/etc/sm64-1024-host.env` (`JUGADORES`, `NOMBRE`, `PUERTO`=7777, `CLAVE`, `MODS`,
  `ADMINS`); después, `sudo systemctl restart sm64-1024-host`
- abrir **UDP 7777** (además de los puertos de coopnet) en el firewall del proveedor
- el coopnet ve al host con una IP local: `COOPNET_IP_PUBLICA` en `/etc/coopnet-1024.env` hace que
  a los que entran les dé la pública (el script la averigua sola)
- **administradores**: nadie escribe en el host, así que el host le da todos sus permisos a las
  identidades de `ADMINS=` (64 caracteres hex, separadas por espacios). Cada uno ve la suya en el
  juego con `/miclave` (también queda en `smnet-identidad.txt`). Un administrador puede echar,
  banear para siempre, nombrar moderadores, cerrar/abrir y sacar los baneos (pausa → Moderación o
  los comandos de `/help`); el host ejecuta sus pedidos
- recomendado: 2 vCPU y 2 GB de RAM; log: `journalctl -u sm64-1024-host -f`

## Telemetría

Cada jugador de una partida masiva manda cada 30 s un informe anónimo (un número de sesión al azar,
sin nombres ni direcciones). Cada minuto el servidor escribe:

- una línea `telemetría: …` en su salida (activos, hosts, coordinadores, SN, NAT, FPS mediana y p10,
  desconexiones, cambios de SN y reentradas del último minuto, tráfico y retransmisiones);
- `telemetria.json` con el último resumen (otra ruta: `COOPNET_TELEMETRIA=/ruta/archivo.json`),
  para mirarlo o servirlo con nginx;
- una línea por minuto en `telemetria.csv` (al lado del JSON) para graficar después.

En el juego, `COOPDX_SMNET_TELEMETRIA=0` la apaga.

## Aviso de versión nueva

El fork no usa el actualizador oficial (instalaría la versión oficial encima). Al arrancar, el juego
le pregunta al servidor cuál es la última versión del fork y, si es más nueva que la suya
(`SMNET_FORK_BUILD` en `ns_smnet_lobby.h`), muestra el aviso con el link:

    COOPNET_FORK_BUILD=2 COOPNET_FORK_VERSION="1024 beta 2" COOPNET_FORK_URL="https://…" ./bin/server

Sin `COOPNET_FORK_BUILD` no avisa nada.

## Límites del encuentro

40 mensajes cada 10 s por dirección y puerto, y 4000 por IP: con CGNAT cientos de jugadores
comparten la IP pública y no se los corta.

## Prueba sin el juego (`dev/prueba_masivo.cpp`)

    g++ -std=c++11 -Icommon -Ilib/include dev/prueba_masivo.cpp bin/libcoopnet.a -Llib/linux -l:libjuice.a -pthread -o bin/prueba_masivo
    ./bin/server &
    ./bin/prueba_masivo host 34197 34198 512 [contraseña] &
    ./bin/prueba_masivo jugador 34197 34198 [contraseña]

El jugador la ve en la lista (`37/512 (masiva)`), recibe `MERR_LOBBY_MASSIVE` al intentar el Join
clásico, mide su NAT, pide entrar y recibe la perforación del host.
