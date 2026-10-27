#!/bin/sh
# Bots en la Mac (también anda en Linux): reparte TOTAL bots en PROCESOS procesos (uno por núcleo)
# con los ajustes livianos, contra un host de otra máquina.
# Desde la carpeta del juego:
#   HOST=192.168.1.41 PLANTILLA=plantilla.bin sh herramientas-1024/bots-mac.sh
# Variables (todas opcionales salvo HOST y PLANTILLA):
#   TOTAL=512 PROCESOS=2 HZ=15 PUERTO=7777 ENTRADA=40 SALTOS=8 CHAT=0 AREA= SEGUNDOS= SNS=
# Ctrl+C corta todos. Cada proceso deja su salida en bots-mac-N.log.
TOTAL=${TOTAL:-512}; PROCESOS=${PROCESOS:-2}; HZ=${HZ:-15}; PUERTO=${PUERTO:-7777}
ENTRADA=${ENTRADA:-40}; SALTOS=${SALTOS:-8}; CHAT=${CHAT:-0}
[ -n "$HOST" ] || { echo "falta HOST=ip del host (la PC con Windows)"; exit 1; }
[ -n "$PLANTILLA" ] && [ -f "$PLANTILLA" ] || { echo "falta PLANTILLA=archivo (la grabada con COOPDX_SMNET_GRABAR, copiala de la PC)"; exit 1; }
BIN=./smnet-mario-bots
# se recompila si falta o si cambió smnet (los bots también pueden ser réplica o coordinador)
if [ ! -x "$BIN" ] || [ -n "$(find lib/smnet/src -newer "$BIN" -name '*.c' 2>/dev/null | head -1)" ]; then
  echo "== compilando los bots"
  sh "$(dirname "$0")/compilar-bots.sh" > /dev/null || exit 1
fi
# cada bot es un socket: macOS deja 256 archivos abiertos por defecto
ulimit -n 10240 2> /dev/null || ulimit -n 4096 2> /dev/null || true
POR=$(( (TOTAL + PROCESOS - 1) / PROCESOS ))
# supernodos en total: uno cada ~16 bots (32 para 512, 64 para 1024); SNS= para cambiarlo
SNS=${SNS:-$(( (TOTAL + 15) / 16 ))}; [ $SNS -gt 64 ] && SNS=64
SN=$(( (SNS + PROCESOS - 1) / PROCESOS )); [ $SN -lt 4 ] && SN=4
PIDS=""
trap 'kill $PIDS 2> /dev/null; exit 0' INT TERM
p=1; RESTAN=$TOTAL
while [ $p -le $PROCESOS ] && [ $RESTAN -gt 0 ]; do
  N=$POR; [ $N -gt $RESTAN ] && N=$RESTAN
  set -- --unirse "$HOST:$PUERTO" --plantilla "$PLANTILLA" --bots "$N" --nombre "Mac$p-" \
         --hz "$HZ" --sn "$SN" --entrada "$ENTRADA" --saltos "$SALTOS"
  [ "$CHAT" != 0 ] && set -- "$@" --chat "$CHAT"
  [ -n "$AREA" ] && set -- "$@" --area "$AREA"
  [ -n "$SEGUNDOS" ] && set -- "$@" --segundos "$SEGUNDOS"
  "$BIN" "$@" > "bots-mac-$p.log" 2>&1 &
  PIDS="$PIDS $!"
  echo "proceso $p: $N bots (Mac$p-*), log bots-mac-$p.log"
  RESTAN=$((RESTAN - N)); p=$((p + 1))
  sleep 1
done
echo "entrando de a uno cada $ENTRADA ms por proceso; Ctrl+C para cortar"
echo "CPU: top -o cpu -pid $(echo $PIDS | tr ' ' ',' | sed 's/,/ -pid /g')"
wait
