#!/bin/sh
# Abre el juego contra el servidor coopnet local de WSL (coopnet/despliegue/local.sh), en MSYS2.
# Busca sola la IP de WSL (cambia cada vez que WSL arranca) y le pasa COOPDX_COOPNET al juego.
#
# Desde juego/sm64coopdx:
#   sh ../herramientas/jugar-local.sh        una instancia
#   sh ../herramientas/jugar-local.sh 2      dos (en una: Host → CoopNet → más de 16; en la otra: Join → CoopNet)
#   SERVIDOR=1.2.3.4:34197 sh ../herramientas/jugar-local.sh 2     otro servidor (por ejemplo el VPS)
# Cada instancia deja su log: smnet-local1.log, smnet-local2.log, ...
N=${1:-1}
EXE=./build/us_pc/sm64coopdx.exe
[ -x "$EXE" ] || EXE=./build/us_pc/sm64coopdx
[ -x "$EXE" ] || { echo "no encuentro el juego en build/us_pc (correlo desde juego/sm64coopdx)"; exit 1; }
if [ -z "$SERVIDOR" ]; then
    IP=""
    WSL=""
    for w in wsl.exe /c/Windows/System32/wsl.exe /c/Windows/Sysnative/wsl.exe; do
        if command -v "$w" > /dev/null 2>&1 || [ -x "$w" ]; then WSL=$w; break; fi
    done
    if [ -n "$WSL" ]; then
        # en modo espejo (networkingMode=mirrored) alcanza 127.0.0.1
        if grep -qsi "networkingMode *= *mirrored" "$USERPROFILE/.wslconfig" "/c/Users/$USERNAME/.wslconfig" 2> /dev/null; then
            IP=127.0.0.1
        else
            # (wsl.exe a veces contesta en UTF-16: se sacan los ceros)
            IP=$("$WSL" -e sh -c "hostname -I" 2> /dev/null | tr -d '\000\r' | awk '{print $1}')
            echo "$IP" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' || IP=""
        fi
    fi
    # la que dejó despliegue/local.sh al arrancar (la más confiable)
    if [ -f /c/Users/Public/coopnet-local.txt ]; then
        F=$(tr -d '\r' < /c/Users/Public/coopnet-local.txt)
        [ -n "$F" ] && { SERVIDOR=$F; IP=${F%:*}; }
    fi
    if [ -z "$IP" ]; then
        echo "no pude averiguar la IP de WSL: en Ubuntu corré  hostname -I  y pasala así:"
        echo "  SERVIDOR=IP:34197 sh ../herramientas/jugar-local.sh $N"
        exit 1
    fi
    [ -n "$SERVIDOR" ] || SERVIDOR="$IP:34197"
fi
echo "servidor coopnet: $SERVIDOR"
# ¿contesta? (TCP del servidor; si no, avisar antes de abrir el juego)
H=${SERVIDOR%:*}; P=${SERVIDOR##*:}
if command -v powershell.exe > /dev/null 2>&1; then
    OK=$(powershell.exe -NoProfile -Command "(New-Object Net.Sockets.TcpClient).ConnectAsync('$H',$P).Wait(1500)" 2> /dev/null | tr -d '\r')
    [ "$OK" = "True" ] || echo "ojo: no contesta $SERVIDOR (¿está corriendo? en WSL: sh despliegue/local.sh --estado)"
fi
i=1
while [ $i -le "$N" ]; do
    NUEVA=""; [ $i -gt 1 ] && NUEVA=1   # las demás con otra identidad (si no, un baneo alcanza a todas)
    COOPDX_SMNET_IDENTIDAD_NUEVA=$NUEVA COOPDX_COOPNET=$SERVIDOR COOPDX_SMNET_LOG=smnet-local$i.log "$EXE" &
    i=$((i + 1))
    sleep 2
done
wait
