#!/bin/sh
# Abre N instancias del juego en modo masivo en esta PC: la primera es el host original
# (COOPDX_SMNET_MASIVO=1) y las demás se unen a ella (COOPDX_SMNET_UNIRSE=127.0.0.1:PUERTO).
# En todas se elige "Host" en el menú (las que se unen ya saben a dónde). Sin el script, lo
# mismo se hace con Join → 127.0.0.1:7777 en las demás.
# Desde juego/sm64coopdx:   sh ../herramientas/abrir-instancias.sh 3
# Cada una deja su log: smnet-inst1.log (host), smnet-inst2.log, ...
N=${1:-2}
PUERTO=${PUERTO:-7777}
EXE=./build/us_pc/sm64coopdx.exe
[ -x "$EXE" ] || EXE=./build/us_pc/sm64coopdx
[ -x "$EXE" ] || { echo "no encuentro el juego en build/us_pc (correlo desde juego/sm64coopdx)"; exit 1; }
COOPDX_SMNET_MASIVO=1 COOPDX_SMNET_LOG=smnet-inst1.log "$EXE" &
echo "instancia 1 (host): elegí Host en el menú y esperá a estar en el castillo; Enter para abrir las demás"
read _
i=2
while [ $i -le $N ]; do
  COOPDX_SMNET_IDENTIDAD_NUEVA=1 COOPDX_SMNET_UNIRSE=127.0.0.1:$PUERTO COOPDX_SMNET_LOG=smnet-inst$i.log "$EXE" &
  echo "instancia $i: elegí Host en el menú"
  i=$((i + 1))
  sleep 2
done
wait
