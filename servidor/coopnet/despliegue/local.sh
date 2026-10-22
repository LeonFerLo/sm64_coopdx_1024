#!/bin/sh
# Servidor coopnet de sm64coopdx-1024 para probar en la PC (Ubuntu en WSL o cualquier Linux).
# Es el mismo servidor que se instala en el VPS (instalar.sh), sin servicio ni usuario aparte.
#
#   sh despliegue/local.sh              compila si hace falta y lo corre acá (Ctrl+C lo para)
#   sh despliegue/local.sh --fondo      lo deja corriendo en segundo plano (log en ~/coopnet-local/server.log)
#   sh despliegue/local.sh --parar      para el de segundo plano
#   sh despliegue/local.sh --estado     si está corriendo, puertos, telemetría y la IP para el juego
#   sh despliegue/local.sh --recompilar fuerza a compilar de cero (después de copiar código nuevo)
#
# Configuración: ~/coopnet-local/local.env (se crea la primera vez; mismas variables que el VPS).
# Telemetría: ~/coopnet-local/telemetria.json y telemetria.csv.
set -e
cd "$(dirname "$0")/.."
[ -f server/smain.cpp ] || { echo "correlo desde la carpeta coopnet (falta server/smain.cpp)"; exit 1; }
DATA="$HOME/coopnet-local"
ENVF="$DATA/local.env"
PIDF="$DATA/server.pid"
LOGF="$DATA/server.log"
mkdir -p "$DATA"

if [ ! -f "$ENVF" ]; then
    cat > "$ENVF" <<EOT
# servidor coopnet local (despliegue/local.sh)
COOPNET_PUERTO=34197
COOPNET_ENCUENTRO=34198
COOPNET_TELEMETRIA=$DATA/telemetria.json
# para probar el aviso de versión nueva (tiene que ser mayor que SMNET_FORK_BUILD del juego):
#COOPNET_FORK_BUILD=2
#COOPNET_FORK_VERSION="1024 beta 2"
#COOPNET_FORK_URL="https://ejemplo.org"
EOT
fi
set -a; . "$ENVF"; set +a
P=${COOPNET_PUERTO:-34197}; U=${COOPNET_ENCUENTRO:-34198}

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2> /dev/null; }

ip_para_juego() {
    # en WSL2 (modo NAT, el de siempre) el juego en Windows no llega por 127.0.0.1 con UDP: usa la IP
    # de WSL; en modo espejo (networkingMode=mirrored en .wslconfig) o en Linux nativo, 127.0.0.1
    if grep -qi microsoft /proc/version 2> /dev/null && ! grep -qsi "networkingMode *= *mirrored" /mnt/c/Users/*/.wslconfig; then
        I=$(hostname -I 2> /dev/null | awk '{print $1}')
        [ -n "$I" ] && { echo "$I"; return; }
    fi
    echo "127.0.0.1"
}

mostrar() {
    I=$(ip_para_juego)
    # para jugar-local.sh (MSYS2): la dirección en una carpeta que ven los dos lados
    [ -d /mnt/c/Users/Public ] && echo "$I:$P" > /mnt/c/Users/Public/coopnet-local.txt 2> /dev/null || true
    echo
    echo "Para el juego (en MSYS2):"
    echo "  COOPDX_COOPNET=$I:$P ./build/us_pc/sm64coopdx.exe"
    echo "o en sm64config.txt:  coopnet_ip_1024 $I   y   coopnet_port_1024 $P"
    [ "$I" != "127.0.0.1" ] && echo "(es la IP de WSL: cambia al reiniciar WSL; con networkingMode=mirrored en .wslconfig alcanza 127.0.0.1)"
    echo "Telemetría: $DATA/telemetria.json (aparece al primer minuto con alguien en una masiva)"
}

tiene_curl() { [ -f /usr/include/curl/curl.h ] || [ -f /usr/include/x86_64-linux-gnu/curl/curl.h ] || [ -f /usr/include/aarch64-linux-gnu/curl/curl.h ]; }

# true si en bin/o hay objetos que no son de Linux (la carpeta vino de MSYS2 con los .o de Windows)
objetos_ajenos() {
    for o in $(find bin/o -name '*.o' 2> /dev/null | head -20); do
        [ "$(head -c 4 "$o" | od -An -c | tr -d ' ')" = "177ELF" ] || return 0
    done
    return 1
}

compilar() {
    if ! command -v g++ > /dev/null 2>&1 || ! command -v make > /dev/null 2>&1 || ! tiene_curl; then
        echo "== instalando lo necesario para compilar (pide la contraseña de sudo)"
        sudo apt-get update -qq && sudo apt-get install -y build-essential libcurl4-openssl-dev
    fi
    if [ -n "$1" ] || objetos_ajenos; then
        echo "== compilando de cero"
        rm -rf bin
    fi
    echo "== compilando (si hace falta)"
    make server -j"$(nproc)" > "$DATA/compilar.log" 2>&1 || { tail -20 "$DATA/compilar.log"; echo "falló la compilación (log: $DATA/compilar.log)"; exit 1; }
}

case "$1" in
    --parar)
        if running; then kill "$(cat "$PIDF")"; rm -f "$PIDF"; echo "parado"; else echo "no estaba corriendo"; fi
        exit 0 ;;
    --estado)
        if running; then echo "corriendo (pid $(cat "$PIDF"), log: $LOGF)"; else echo "no está corriendo en segundo plano"; fi
        ss -lntup 2> /dev/null | grep -E ":($P|$U|$((U + 1)))\b" || true
        [ -f "$DATA/telemetria.json" ] && { echo; cat "$DATA/telemetria.json"; }
        mostrar; exit 0 ;;
esac

if running; then echo "ya está corriendo en segundo plano (pid $(cat "$PIDF")); --parar para pararlo"; exit 1; fi
REB=""; [ "$1" = "--recompilar" ] && REB=1
compilar "$REB"
cd "$DATA"
if [ "$1" = "--fondo" ]; then
    nohup "$OLDPWD/bin/server" >> "$LOGF" 2>&1 &
    echo $! > "$PIDF"
    sleep 1
    running || { tail -5 "$LOGF"; echo "no arrancó"; exit 1; }
    tail -3 "$LOGF"
    echo "corriendo en segundo plano (pid $(cat "$PIDF")); log: tail -f $LOGF"
    mostrar
else
    mostrar
    echo
    echo "== servidor (Ctrl+C para parar)"
    exec "$OLDPWD/bin/server"
fi
