#!/usr/bin/env bash
# preparar_entorno.sh — Crea los archivos de prueba de confinamiento junto a la
# raíz web del servidor original (Laboratorio 2, USB).
# Uso: ./preparar_entorno.sh <carpeta del C-Web-Server clonado>
# Todos los "secretos" son FICTICIOS.
set -euo pipefail
D="${1:?Indique la carpeta del repositorio C-Web-Server}"
cd "$D"
echo "SECRETO: clave_bd=Usb2026! (fuera de public_html)"        > secreto.txt
mkdir -p public_html.bak
echo "RESPALDO: usuario=admin clave=respaldo123"                > public_html.bak/credenciales.txt
mkdir -p public_html/docs
echo "<p>documento legitimo</p>"                                > public_html/docs/manual.html
echo "<p>version 2</p>"                                         > "public_html/notas..v2.html"
ln -sfn /etc public_html/enlace_etc
echo "Entorno de pruebas listo en $(pwd):"
find . -maxdepth 2 -not -path './.git*' | sort
