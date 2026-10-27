#!/bin/sh
# Compila libcoopnet (nuestra versión, con partidas masivas: ../../coopnet) y la copia al juego.
# Desde la carpeta del juego, en MSYS2 MINGW64 (Windows), Linux o Mac:   sh herramientas-1024/compilar-coopnet.sh
# Hace falta porque libcoopnet.h cambió (callback OnMassiveLobby): con la biblioteca original el
# juego compila pero se rompe al usar CoopNet.
set -e
J=$(pwd)
H=$(cd "$(dirname "$0")" && pwd)
if [ -d "$H/../servidor/coopnet" ]; then C=$(cd "$H/../servidor/coopnet" && pwd); else C=$(cd "$H/../../coopnet" && pwd); fi
[ -d "$J/lib/coopnet" ] || J=$(cd "$(dirname "$0")/.." && pwd)
cd "$C"
if [ "$(uname -s)" = Darwin ]; then
  # en Mac el juego usa la biblioteca dinámica (libcoopnet.dylib) al lado de libjuice
  if [ "$(uname -m)" = arm64 ]; then D=mac_arm; A="OSX_ARM=1"; else D=mac_intel; A=""; fi
  MK=make; command -v gmake > /dev/null && MK=gmake
  rm -rf bin
  $MK dynlib OSX_BUILD=1 $A
  cp bin/libcoopnet.dylib "$J/lib/coopnet/$D/libcoopnet.dylib"
  cp common/libcoopnet.h "$J/lib/coopnet/include/libcoopnet.h"
  echo "listo: lib/coopnet/$D/libcoopnet.dylib (ahora recompilá el juego)"
  exit 0
fi
make lib
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) D=win64 ;;
  *) D=linux ;;
esac
cp bin/libcoopnet.a "$J/lib/coopnet/$D/libcoopnet.a"
cp common/libcoopnet.h "$J/lib/coopnet/include/libcoopnet.h"
echo "listo: lib/coopnet/$D/libcoopnet.a (ahora recompilá el juego)"
