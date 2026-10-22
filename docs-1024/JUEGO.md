# juego/ — fork de sm64coopdx con `smnet`

El fork se guarda como **parches** sobre un commit fijado del original (ver `obtener.sh`), no como
copia del juego: el repo queda chico y los parches se pueden rehacer sobre versiones nuevas.

```bash
bash juego/obtener.sh                 # baja sm64coopdx 8cd6e59, copia smnet y aplica parches/
cp ~/baserom.us.z64 juego/sm64coopdx/ # tu propia ROM (no se incluye ni se sube)
cd juego/sm64coopdx && make -j8 SMNET=1   # igual que el original, con la red de smnet
```

Para hacer un parche nuevo: cambiar el código en `juego/sm64coopdx/`, y después
`git -C juego/sm64coopdx diff > juego/parches/NN-nombre.patch`.

Diseño y orden de trabajo: [`../INTEGRACION.md`](../INTEGRACION.md).

## Parches

| Parche | Qué hace |
|---|---|
| `01-ns-smnet-transporte.patch` | `NS_SMNET`: la conexión directa (Host / Join por IP) usa smnet en vez de sockets. Los paquetes del juego van como mensajes de smnet (cliente ↔ host), cifrados y con perforación de NAT. Hasta 16 jugadores, como el original. El estado de Mario (`PACKET_PLAYER`) no pasa por el host: va por el flujo de estado de smnet, directo a los jugadores de la misma área. `COOPDX_SMNET=0` vuelve a sockets; `COOPDX_SMNET_IP=<ip pública>` en el host si se hostea por Internet. Deja `smnet-host.log` / `smnet-cliente.log` en la carpeta desde donde se corre el juego (estado cada segundo, paquetes enviados y recibidos). |

## Modo masivo (más de 16 jugadores)

Desde el menú, como en el juego original:

- **Host** abre una partida masiva (en el puerto de Host, 7777 por defecto).
- **Join** con la IP (y el puerto, `ip:puerto`) se suma a una partida masiva. El que se une
  arranca enseguida en su propio mundo y entra a la red de los demás; en el chat avisa cuando se
  conectó, o si el otro no responde (sigue intentando).

**El host.** Cada instalación tiene una clave propia (`smnet-host.key`, en la carpeta del
usuario) que firma la lista de mods de la partida y el aviso "soy el host". El que se une aprende
la clave con la lista de mods y la recuerda para esa dirección (`smnet-partidas-conocidas.txt`; si
otra vez viene otra clave, no entra y avisa). En la pausa:

- el host tiene **Ajustes de partida**, **Salir de la partida** (la partida sigue sin él; si le
  vuelve a dar Host, vuelve a entrar a esa misma partida y sigue siendo el host: gGlobalSyncTable,
  la lista de mods y finalizar son suyos aunque otro haya quedado de coordinador) y **Finalizar
  partida** (con confirmación: todos pierden la conexión y vuelven al menú);
- los que se unieron no ven los ajustes de partida y tienen **Desconectarse**.

El host guarda a quién volver en `smnet-partida-host.txt` (12 h; lo borra Finalizar). En una
partida chica nadie toma el control cuando sale el host: los demás se quedan reintentando y, si
vuelve, abre la partida en su puerto y entran solos. `COOPDX_SMNET_PARTIDA_NUEVA=1` hace que Host
abra una partida nueva sin volver a la anterior.

Unirse activa los mods de la partida; los que tenía el usuario se guardan
(`smnet-mods-propios.txt`) y vuelven al salir (o al abrir el juego, si se cerró adentro).

**Partidas masivas en la lista de CoopNet** (necesita nuestro servidor coopnet, `../coopnet`, ver
`coopnet/MASIVO.md`, y `libcoopnet` recompilada: `sh ../herramientas/compilar-coopnet.sh`):

- Host con **CoopNet** y **más de 16** jugadores (hasta 1024, en Ajustes del host) abre una partida
  masiva y la lista en coopnet; con 16 o menos es una partida de coopnet clásica.
- En la lista de partidas (públicas o privadas) las masivas se ven como `37/512` (máximo de más de 16); al
  elegir una, el juego le pregunta al encuentro del servidor dónde está el host (que perfora su NAT
  hacia uno), mide el tipo de NAT y sigue con el Join masivo (bajada de mods incluida).
- El servidor es el de `coopnet_ip`/`coopnet_port` en `sm64config.txt`; `COOPDX_COOPNET=ip:puerto`
  lo cambia para probar.
- Telemetría anónima de las masivas al servidor cada 30 s (ver `coopnet/MASIVO.md`);
  `COOPDX_SMNET_TELEMETRIA=0` la apaga.
- Sin actualizador oficial (`UPDATER=0` con `SMNET=1`): el aviso de versión nueva lo da el servidor
  coopnet (`SMNET_FORK_BUILD` / `SMNET_FORK_LABEL` en `pc/network/version.h`).
- Versión propia: `get_version()` da `v1.5.1-1024b1` (`SMNET_FORK_TAG`), así la lista y el Join no
  mezclan este cliente con el oficial; los mods siguen viendo `SM64COOPDX_VERSION` = `v1.5.1`.
- Servidor por defecto: `make SMNET=1 COOPNET_SERVIDOR=host[:puerto]` (sin eso, el oficial). El fork
  guarda el suyo en `coopnet_ip_1024` / `coopnet_port_1024` de `sm64config.txt` (la carpeta de
  configuración es la misma del sm64coopdx oficial y así no se pisan).

`COOPDX_SMNET_MASIVO=0` vuelve al modo clásico de hasta 16 (Host / Join de siempre). Siguen
valiendo `COOPDX_SMNET_UNIRSE=ip[:puerto]` (unirse aunque se elija Host) y
`sh ../herramientas/abrir-instancias.sh 3` para abrir varias instancias en la misma PC (cada una
deja `smnet-instN.log`; `COOPDX_SMNET_LOG=archivo` elige el log a mano).

Cada uno ve como Marios completos a los 15 más cercanos de su misma área (slots con histéresis).
El resto de los que están cerca se dibujan como impostores (un Mario sin física, con su
personaje y sus colores) en tres niveles según la distancia:

| Nivel | Cuántos (variable) | Cómo se ve |
|---|---|---|
| medio | 64 (`COOPDX_SMNET_IMPOSTORES_MARIO`) | animado, con sombra y parpadeo |
| bajo | 96 (`COOPDX_SMNET_IMPOSTORES_BAJOS`) | Mario de pocas caras (cajas con sus colores, 132 triángulos) animado a 10 Hz, sin sombra; `COOPDX_SMNET_BAJOS_COMPLETOS=1` para el modelo completo |
| lejano | el resto, hasta el tope del nivel de Multitud o `COOPDX_SMNET_IMPOSTORES` | el de pocas caras animado a 5 Hz, sin sombra; `COOPDX_SMNET_LEJANOS_QUIETOS=1` quietos, `COOPDX_SMNET_LEJANOS_COMPLETOS=1` para el modelo completo, `COOPDX_SMNET_LEJANOS_GORRA=1` para la gorra |

El piso de cada impostor se vuelve a buscar solo si se movió (o cada 3 s): un grupo parado casi no
cuesta CPU.

Cuántos se dibujan en cada nivel se elige en Opciones > Pantalla > **Multitud** (Muy baja a Muy alta: medios/bajos y tope total 16/32/384, 32/64/512, 64/96/768, 96/128 y 128/192 sin tope; los lejanos son el resto); las variables de arriba, si están, mandan sobre el menú.

La lista de TAB es global (todos los de la partida); se recorre con la rueda del ratón o RePág/AvPág.
El chat es global (llega a todos, estén donde estén). Enemigos, cajas, monedas y estrellas se comparten en la misma área: cada objeto lo simula el que está más cerca (tenga o no slot en los demás).
`COOPDX_SMNET_SUBIDA=<kbit/s>` es la subida que declara cada uno (por defecto 30000): por debajo
de 20000 no puede ser SN.

## Bots que se ven en el juego (`herramientas/`)

`smnet-mario-bots` mete N jugadores falsos en una partida en modo masivo. Repiten un `PLAYER`
real grabado del juego, cada uno caminando en círculos con su nombre y colores: para el juego
son jugadores como cualquiera (ocupan slots, se los puede golpear).

1. Grabar la plantilla: abrir el host con `COOPDX_SMNET_GRABAR=plantilla.bin` además de
   `COOPDX_SMNET_MASIVO=1`, ir a un lugar plano y abierto y **correr** unos segundos (se graba
   solo mientras Mario corre, una vez por segundo).
2. Compilar, desde `juego/sm64coopdx`: `sh ../herramientas/compilar-bots.sh`.
3. Correr: `./smnet-mario-bots --unirse 127.0.0.1:7777 --plantilla plantilla.bin --bots 30`
   (`--radio` cuánto se reparten alrededor del lugar grabado, `--vuelta` el tamaño de sus
   círculos, `--segundos` para que terminen solos, `--chat S` para que cada bot escriba en el chat cada S segundos).
   Para ponerlos en otro lugar: `--area NIVEL[:ÁREA[:ACTO]]` (`jardines`, `castillo`, `patio`,
   `bob`, `wf`, `jrb`, `ccm`, `bbh`, `hmc`, `lll`, `ssl`, `ddd`, `sl`, `wdw`, `ttm`, `thi`, `ttc`,
   `rr`, `bitdw`, `bitfs`, `bits`, `pss`, `cotmc`, `totwc`, `vcutm`, `wmotr`, `sa`, o
   `NÚMERO/CURSO`; el acto por defecto es 1 en los cursos y 0 en el castillo) y
   `--centro X,Y,Z` (dónde se reparten; sin eso, donde se grabó la plantilla). Ejemplo:
   `--area bob:1:1 --centro -6500,0,6500`. Los bots no simulan objetos: nunca son dueños de nada.

## Simulador de descarga de mods (`herramientas/mod_clientes.c`)

`sh ../herramientas/compilar-bots.sh` también compila `smnet-mod-clientes`. Una semilla (que
hace de host o se suma a una partida) reparte una carpeta de mods; los clientes falsos piden el
manifiesto, bajan lo que les falta de todas las semillas que encuentran y verifican la huella de
cada archivo.

    ./smnet-mod-clientes --sembrar ../mods --puerto 7778 --tasa 4000 --atender 4
    ./smnet-mod-clientes --clientes 20 --unirse 127.0.0.1:7778 --guardar bajado --semillas

`--mods a,b` elige mods de la carpeta (si no, todos), `--atender N` cuántos atiende a la vez cada
semilla (el resto espera en una lista, con su posición), `--tasa` pedazos de 1 KB por segundo por
semilla, `--semillas` hace que los clientes repartan a los demás lo que ya bajaron (cada bloque de 64 KB, apenas se verifica, aunque no hayan terminado), `--guardar` escribe
lo bajado en `CARPETA/clienteN/` (un archivo que ya está bien no se baja), `--corromper` hace una
semilla que manda basura (para ver la verificación). Cada segundo muestra listos, bajando, en
espera, fallados y lo que mandó cada semilla.

## API de Lua para mods (modo masivo)

Los índices de sm64coopdx (0..15) son slots que rotan. Para identificar a cada jugador de forma
estable, y ver a todos los de la partida, los mods tienen:

| Función | Devuelve |
|---|---|
| `smnet_active()` | `true` en modo masivo |
| `smnet_local_id()` | el id propio (0..1023) |
| `smnet_player_count()` | jugadores en la partida, contándose |
| `smnet_players()` | tabla con los ids de los demás |
| `smnet_player_name(id)` | nombre |
| `smnet_player_slot(id)` | índice local (0 = uno mismo, 1..15) o `nil` si no tiene slot |
| `smnet_slot_player(indice)` | id de quien ocupa ese índice local |
| `smnet_player_pos(id)` | `x, y, z` si se lo ve en el área |
| `smnet_send(id, texto)` | mensaje de mod a un jugador, o a todos con `id = -1` (≤ 1000 B) |
| `smnet_on_message(fn)` | `fn(id_origen, texto)` al llegar un mensaje de mod |
| `smnet_on_player(fn)` | `fn(id, entro)` cuando alguien entra o se va (cualquiera de la partida) |

Ejemplo: `herramientas/mod-smnet-ejemplo/` (comandos `/jugadores` y `/saludar`). Se copia la
carpeta a `%APPDATA%\sm64coopdx\mods\` y se activa en el menú de mods al hostear.

**Moderación de las partidas masivas** (`ns_smnet_mod.c`): el host (y los moderadores que nombra)
desde pausa → **Moderación** (buscar entre todos los jugadores, echar, banear, silenciar, cerrar la
entrada) o por chat: `/jugadores [texto]`, `/kick`, `/ban`, `/permban`, `/mute`, `/unmute`,
`/moderator`, `/cerrar`, `/abrir`, `/desbanear` (`/help` los lista). Echado: no vuelve por 5 min;
baneado: por toda la partida; `/permban`: para siempre (`smnet-baneados.txt` del host). Todos
descartan a los echados y el coordinador de smnet bloquea su identidad (`smnet-identidad.key`,
persistente; `COOPDX_SMNET_IDENTIDAD_NUEVA=1` usa otra, para probar varias instancias en una PC).
Cerrada: solo vuelven los que ya estaban y en la lista figura llena.
En un host dedicado (`coopnet/despliegue/host-dedicado.sh`) los **administradores** son identidades
de smnet (`COOPDX_SMNET_ADMINS` en el host; cada jugador ve la suya con `/miclave`) que tienen los
permisos del host: el host los reconoce al entrar, los anuncia en APP_MODLIST y ejecuta sus pedidos.
El host también arranca solo desde la línea de comandos: `--server PUERTO --coopnet CLAVE
--playercount N` con N > 16 abre la partida masiva listada en coopnet (con `--headless`, sin pantalla).

**Ajustes → Partida masiva**: multitud, cómo se ven los lejanos (pocas caras / gorra / completos,
animados o no), medianos completos, "Mi conexión: lenta" (no reparte tráfico de otros) y las
estadísticas anónimas. Las variables `COOPDX_SMNET_*` de antes siguen mandando si están puestas.

