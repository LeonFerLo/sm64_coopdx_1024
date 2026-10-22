#!/bin/sh
# Compila smnet-mario-bots con la copia de smnet que tiene el juego (lib/smnet).
# Desde juego/sm64coopdx:   sh ../herramientas/compilar-bots.sh
# En Linux y en MSYS2 MINGW64 (Windows) por igual.
set -e
H=$(cd "$(dirname "$0")" && pwd)
L=lib/smnet
[ -d "$L/src" ] || { echo "correlo desde juego/sm64coopdx (falta $L)"; exit 1; }
EXTRA="-lm"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXTRA="-lws2_32 -lbcrypt"; OUT=smnet-mario-bots.exe ;; *) OUT=smnet-mario-bots ;; esac
gcc -O2 -std=gnu11 -I "$L/include" -I "$L/src" -I "$L/third_party/monocypher" \
    "$H/mario_bots.c" "$L"/src/*.c "$L/third_party/monocypher/monocypher.c" -lz $EXTRA -o "$OUT"
echo "listo: $OUT"
# simulador de la descarga de mods (clientes falsos y semillas)
OUT2=smnet-mod-clientes; case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) OUT2=smnet-mod-clientes.exe ;; esac
gcc -O2 -std=gnu11 -I "$L/include" -I "$L/src" -I "$L/third_party/monocypher" -I src/pc/network/smnet \
    "$H/mod_clientes.c" src/pc/network/smnet/modsync.c "$L"/src/*.c "$L/third_party/monocypher/monocypher.c" -lz $EXTRA -o "$OUT2"
echo "listo: $OUT2"
