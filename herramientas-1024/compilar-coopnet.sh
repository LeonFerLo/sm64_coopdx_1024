#!/bin/sh
# Compila libcoopnet (nuestra versión, con partidas masivas: ../../coopnet) y la copia al juego.
# Desde juego/sm64coopdx, en MSYS2 MINGW64 (Windows) o Linux:   sh ../herramientas/compilar-coopnet.sh
# Hace falta porque libcoopnet.h cambió (callback OnMassiveLobby): con la biblioteca original el
# juego compila pero se rompe al usar CoopNet.
set -e
J=$(pwd)
C=$(cd "$(dirname "$0")/../servidor/coopnet" 2>/dev/null && pwd || cd "$(dirname "$0")/../../coopnet" && pwd)
[ -d "$J/lib/coopnet" ] || J=$(cd "$(dirname "$0")/.." && pwd)
cd "$C"
make lib
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) D=win64 ;;
  Darwin) D=mac_intel ;;
  *) D=linux ;;
esac
cp bin/libcoopnet.a "$J/lib/coopnet/$D/libcoopnet.a"
cp common/libcoopnet.h "$J/lib/coopnet/include/libcoopnet.h"
echo "listo: lib/coopnet/$D/libcoopnet.a (ahora recompilá el juego)"
