#!/usr/bin/env bash
# instalar_wsl.sh — Deja el servidor reforzado en línea desde WSL2 (con systemd)
# y lo publica en Internet con un túnel bore. Ejecutar con: sudo bash wsl/instalar_wsl.sh
# Sólo usa los secretos FICTICIOS de preparar_entorno.sh.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Ejecute con sudo"; exit 1; }
REPO="$(cd "$(dirname "$0")/.." && pwd)"

BORE_VER=v0.6.0
BORE_SHA=60548b7a145ba334981b19fda7cd0210d24a108a3d0dc113919b33fd2eaa90ab

echo "== 1. Copiar y compilar el servidor en /opt/lab2"
install -d /opt/lab2/public_html
install -m 644 "$REPO/seguro/server_seguro.c" "$REPO/seguro/Makefile" /opt/lab2/
make -C /opt/lab2
echo '<h1>Laboratorio 2 - Servidor reforzado</h1>' > /opt/lab2/public_html/index.html
bash "$REPO/preparar_entorno.sh" /opt/lab2
chmod -R a+rX /opt/lab2

echo "== 2. Instalar bore $BORE_VER (verificando SHA-256)"
TMP="$(mktemp -d)"
curl -fsSL -o "$TMP/bore.tgz" \
  "https://github.com/ekzhang/bore/releases/download/$BORE_VER/bore-$BORE_VER-x86_64-unknown-linux-musl.tar.gz"
tar -xzf "$TMP/bore.tgz" -C "$TMP"
echo "$BORE_SHA  $TMP/bore" | sha256sum -c -
install -m 755 "$TMP/bore" /usr/local/bin/bore
rm -rf "$TMP"

echo "== 3. Instalar y arrancar los servicios"
install -m 644 "$REPO/seguro/lab2-servidor.service" /etc/systemd/system/
install -m 644 "$REPO/wsl/lab2-tunel.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now lab2-servidor lab2-tunel
sleep 3
systemctl --no-pager status lab2-servidor lab2-tunel | head -30
ps -o user,cmd -C server_seguro
