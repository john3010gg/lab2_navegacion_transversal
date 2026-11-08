#!/usr/bin/env bash
# pruebas.sh — Batería de pruebas de confinamiento (Laboratorio 2, USB)
# Uso: ./pruebas.sh [host] [puerto]
# Envía peticiones HTTP "en crudo" con netcat para que el cliente NO normalice
# la ruta (curl y los navegadores eliminan los "../" antes de enviarlos).
# Ejecutar SOLO contra un servidor propio en localhost o en su máquina de pruebas.

export LC_ALL=C.UTF-8   # para que ${#var} cuente caracteres y no bytes
HOST="${1:-localhost}"
PORT="${2:-2001}"

# Envía "GET <ruta> HTTP/1.1" y devuelve la respuesta completa
pedir() {
  printf 'GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$1" "$HOST" \
    | nc -w 3 "$HOST" "$PORT" 2>/dev/null
}

# Extrae el código de estado y una pista del contenido devuelto
resumir() {
  local resp="$1" codigo pista
  codigo=$(printf '%s' "$resp" | head -1 | awk '{print $2}')
  [ -z "$codigo" ] && codigo="(sin respuesta)"
  pista=$(printf '%s' "$resp" | grep -o -m1 -E 'SECRETO[^<]*|RESPALDO[^<]*|root:x:0:0[^<]*|documento legitimo|version 2|Well Done|Woops[^<]*|400 Bad Request|Not Found|Forbidden' | head -1)
  [ -z "$pista" ] && pista="(cuerpo vacío)"
  printf '%-6s %s\n' "$codigo" "$pista"
}

# Rellena con espacios contando caracteres (printf de bash cuenta bytes y descuadra los acentos)
pad() { local s="$1" w="$2" n=${#1}; printf '%s%*s' "$s" $(( w > n ? w - n : 0 )) ''; }

caso() {   # caso <id> <descripción> <ruta>
  printf '%s %s %s ' "$(pad "$1" 4)" "$(pad "$2" 36)" "$(pad "$3" 31)"
  resumir "$(pedir "$3")"
}

echo "Servidor: $HOST:$PORT    Fecha: $(date '+%Y-%m-%d %H:%M:%S')"
printf '%s %s %s %s %s\n' "$(pad ID 4)" "$(pad Prueba 36)" "$(pad 'Ruta enviada' 31)" "$(pad Código 6)" "Contenido devuelto"
printf '%.0s-' {1..118}; echo
caso P01 "Raíz (petición legítima)"            "/"
caso P02 "Archivo legítimo"                    "/index.html"
caso P03 "Archivo en subdirectorio"            "/docs/manual.html"
caso P04 "Archivo inexistente"                 "/noexiste.html"
caso P05 "Recorrido ../ un nivel"              "/../secreto.txt"
caso P06 "Recorrido ../ hasta /etc/passwd"     "/../../../../../../etc/passwd"
caso P07 "../ disfrazado con ./ y //"          "/./docs/./../../secreto.txt"
caso P08 "Codificación %2e%2e"                 "/%2e%2e/secreto.txt"
caso P09 "Codificación mixta .%2e"             "/.%2e/secreto.txt"
caso P10 "Doble codificación %252e"            "/%252e%252e/secreto.txt"
caso P11 "Barra codificada ..%2f"              "/..%2fsecreto.txt"
caso P12 "Barra invertida ..\\"                '/..\secreto.txt'
caso P13 "Ruta absoluta //etc/passwd"          "//etc/passwd"
caso P14 "Byte nulo %00"                       "/index.html%00.txt"
caso P15 "Prefijo hermano (sin /inicial)"      ".bak/credenciales.txt"
caso P16 "Enlace simbólico dentro de la raíz"  "/enlace_etc/passwd"
caso P17 "Nombre legítimo que contiene .."     "/notas..v2.html"
caso P18 "Cadena de consulta ?x=1"             "/index.html?x=1"
caso P19 "Directorio sin barra final"          "/docs"
