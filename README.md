# sm64coopdx-1024 (beta 1, inestable)

Fork de [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) para **partidas masivas de hasta
1024 jugadores**. La red de las partidas masivas es **smnet**, propia, P2P con supernodos. Cada
jugador ve completos a los 15 más cercanos y al resto como "impostores" livianos. Las partidas
normales (hasta 16) siguen funcionando igual que en el original.

> ⚠️ **Beta inestable y sin pruebas masivas.** Puede cortarse, desincronizarse o cerrarse.
> Las estadísticas anónimas (FPS, ping y cantidad de jugadores, sin datos personales) ayudan a
> mejorarla. Se apagan en Opciones → Partida masiva.

**La ROM no se incluye.** Necesitás tu propia copia de Super Mario 64 (US, `baserom.us.z64`).

## Jugar

1. Bajá la última versión de [Releases](../../releases) y descomprimila.
2. Abrí el juego y elegí tu ROM cuando la pida.
3. Join → CoopNet → entrá a una partida que muestre más de 16 jugadores.

## Qué tiene

- Partidas masivas listadas en CoopNet. Host con más de 16 jugadores abre una.
- Moderación (pausa → Moderación, o `/help` en el chat): echar, banear, silenciar, moderadores,
  cerrar la partida y administradores para hosts dedicados (`/miclave`).
- Opciones → Partida masiva: multitud, jugadores lejanos, conexión lenta, estadísticas.
- Aviso de versión nueva propio, sin el actualizador del original.

## Compilar

Igual que sm64coopdx, agregando `SMNET=1`:

```sh
make -j4 SMNET=1                                   # Windows (MSYS2 MINGW64) o Linux
make -j4 SMNET=1 COOPNET_SERVIDOR=ip[:puerto]      # apuntando a tu servidor coopnet
```

`lib/coopnet` trae la libcoopnet modificada. Si cambiás `servidor/coopnet`, rehacela con
`sh herramientas-1024/compilar-coopnet.sh`.

## Estructura

| Carpeta | Qué es |
|---|---|
| (raíz) | el juego: sm64coopdx con los cambios (`src/pc/network/smnet/`, menús en `src/pc/djui/`) |
| `lib/smnet/` | la biblioteca de red smnet |
| `servidor/coopnet/` | el servidor coopnet modificado (partidas masivas, encuentro UDP, telemetría) y `despliegue/` para el VPS (servidor + host dedicado) |
| `herramientas-1024/` | bots de prueba y scripts |
| `docs-1024/` | documentación técnica ([juego](docs-1024/JUEGO.md), [servidor](docs-1024/SERVIDOR.md)) |

## Servidor propio

En un VPS con Ubuntu o Debian, desde la carpeta del repositorio:

```sh
sudo sh servidor/coopnet/despliegue/instalar.sh
sudo sh servidor/coopnet/despliegue/host-dedicado.sh /ruta/a/baserom.us.z64
```

Los detalles están en [docs-1024/SERVIDOR.md](docs-1024/SERVIDOR.md).

## Créditos y licencias

- sm64coopdx y coopnet: sus autores originales (ver [README-original.md](README-original.md) y
  `credits.txt`). coopnet está bajo AGPL-3.0 (`servidor/coopnet/LICENSE`).
- Basado en la decompilación de Super Mario 64. Este proyecto no está afiliado a Nintendo y no
  distribuye ningún contenido del juego.
