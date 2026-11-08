# Laboratorio 2 — Navegación transversal en el C Web Server

Material de apoyo del informe. Universidad Simón Bolívar, Dpto. de Computación y TI.

## Contenido

| Ruta | Qué es |
| --- | --- |
| `seguro/server_seguro.c` | Servidor reconstruido con 6 capas de confinamiento |
| `seguro/Makefile` | Compilación con advertencias y endurecimiento del binario |
| `seguro/lab2-servidor.service` | Unidad systemd para dejarlo en línea |
| `pruebas.sh` | Batería de 19 peticiones de confinamiento (usa `nc`) |
| `preparar_entorno.sh` | Crea los archivos de prueba (secretos ficticios) |
| `render_capturas.py` | Convierte las salidas de terminal en imágenes |
| `evidencia/` | Salidas reales de cada experimento (texto) |
| `capturas/` | Figuras 1 a 13 del informe |

## Reproducir los experimentos (Linux ≥ 5.6, p. ej. Ubuntu 22.04 o 24.04)

```bash
sudo apt install build-essential git curl netcat-openbsd strace
git clone https://github.com/AaronKalair/C-Web-Server.git original
./preparar_entorno.sh original

# 1) ANTES: servidor original (puerto 2001 fijo, raíz relativa al directorio actual)
cd original && make && ./server &          # otra terminal:
cd .. && ./pruebas.sh localhost 2001
pkill -x server

# 2) DESPUÉS: servidor reforzado sobre la MISMA raíz web
cd seguro && make
sudo ./server_seguro -r ../original/public_html &    # con sudo: chroot + usuario nobody
cd .. && ./pruebas.sh localhost 2001

# 3) Capa 4 aislada (sólo demostración: sin filtro textual ni chroot)
./seguro/server_seguro -p 2003 -n -x -r original/public_html &
printf 'GET /../secreto.txt HTTP/1.1\r\n\r\n' | nc localhost 2003
```

Ejecute las pruebas sólo contra servidores propios, en `localhost` o en su máquina de pruebas.

## Dejarlo en línea para la evaluación

1. Cree una máquina virtual Linux (≥ 5.6) en el proveedor que prefiera y abra **sólo** el puerto 8080 en su cortafuegos.
2. Copie `seguro/` y una raíz web de demostración a `/opt/lab2/` (`/opt/lab2/public_html`), compile con `make` y copie `lab2-servidor.service` a `/etc/systemd/system/`.
3. `sudo systemctl daemon-reload && sudo systemctl enable --now lab2-servidor`
4. Entregue al profesor `http://<IP>:8080/` y el script `pruebas.sh` (`./pruebas.sh <IP> 8080`).

No publique nunca el servidor original en Internet: cae con 11 conexiones vacías y filtra archivos.
Como alternativa sin servidor, publique este material en un *fork* de GitHub para que el profesor lo compile.

## Créditos

Este trabajo analiza y reconstruye el **C Web Server** de Aaron Kalair:
<https://github.com/AaronKalair/C-Web-Server>. `seguro/server_seguro.c` es una versión
reforzada basada en ese proyecto; el código original pertenece a su autor.

Autores del laboratorio: John Garrido (20-10293) y Andrés Ramírez (21-10520),
Universidad Simón Bolívar.
