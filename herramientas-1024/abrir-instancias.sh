#!/bin/sh
# Abre N instancias del juego en modo masivo en esta PC: la primera es el host original
# (COOPDX_SMNET_MASIVO=1) y las demás se unen a ella (COOPDX_SMNET_UNIRSE=127.0.0.1:PUERTO).
# En todas se elige "Host" en el menú (las que se unen ya saben a dónde). Sin el script, lo
# mismo se hace con Join → 127.0.0.1:7777 en las demás.
# Desde la carpeta del juego:   sh herramientas-1024/abrir-instancias.sh 3
# Para sumarse a un host de OTRA máquina (todas se unen, ninguna es host):
#   HOST=192.168.1.41 sh herramientas-1024/abrir-instancias.sh 4
# Para entrar por la lista de CoopNet de un servidor coopnet de la red (en vez de por IP):
#   COOPNET=192.168.1.41 sh herramientas-1024/abrir-instancias.sh 4     (y en cada una Join → CoopNet)
# Cada una deja su log: smnet-inst1.log (host), smnet-inst2.log, ...
N=${1:-2}
PUERTO=${PUERTO:-7777}
EXE=${EXE:-./build/us_pc/sm64coopdx.exe}
[ -x "$EXE" ] || EXE=./build/us_pc/sm64coopdx
[ -x "$EXE" ] || EXE=./build/us_pc/sm64coopdx.app/Contents/MacOS/sm64coopdx   # Mac: el Makefile lo mueve al .app
[ -x "$EXE" ] || { echo "no encuentro el juego en build/us_pc (correlo desde la carpeta del juego, después de compilar)"; exit 1; }
if [ -n "$COOPNET" ]; then
  case "$COOPNET" in *:*) ;; *) COOPNET=$COOPNET:34197 ;; esac
  export COOPDX_COOPNET=$COOPNET
  i=1
  while [ $i -le $N ]; do
    COOPDX_SMNET_IDENTIDAD_NUEVA=1 COOPDX_SMNET_LOG=smnet-inst$i.log "$EXE" &
    echo "instancia $i: Join → CoopNet (servidor $COOPNET)"
    i=$((i + 1)); sleep 2
  done
  wait; exit 0
fi
if [ -n "$HOST" ]; then
  i=1; DESTINO=$HOST:$PUERTO
  echo "uniéndose a $DESTINO"
else
  COOPDX_SMNET_MASIVO=1 COOPDX_SMNET_LOG=smnet-inst1.log "$EXE" &
  echo "instancia 1 (host): elegí Host en el menú y esperá a estar en el castillo; Enter para abrir las demás"
  read _
  i=2; DESTINO=127.0.0.1:$PUERTO
fi
while [ $i -le $N ]; do
  COOPDX_SMNET_IDENTIDAD_NUEVA=1 COOPDX_SMNET_UNIRSE=$DESTINO COOPDX_SMNET_LOG=smnet-inst$i.log "$EXE" &
  echo "instancia $i: elegí Host en el menú"
  i=$((i + 1))
  sleep 2
done
wait
