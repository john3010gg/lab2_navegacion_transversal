/*
 * server_seguro.c
 * ---------------------------------------------------------------------------
 * Versión reforzada del "C Web Server" de Aaron Kalair
 * (https://github.com/AaronKalair/C-Web-Server, commit 3ca1a22, 2012).
 *
 * Laboratorio Nro. 2 — Problemas con la navegación transversal en la red
 * Universidad Simón Bolívar — Dpto. de Computación y Tecnología de la Información
 *
 * Se conserva la arquitectura original (pool de procesos pre-creados que
 * comparten el socket de escucha y un contador de bytes en memoria compartida),
 * pero el control de acceso a archivos se RECONSTRUYE por completo, en capas:
 *
 *   Capa 1  Validación estricta de la línea de petición: método (GET/HEAD),
 *           versión HTTP/1.0 o HTTP/1.1, objetivo que empieza por '/',
 *           límites de longitud y tiempo de lectura.
 *   Capa 2  Decodificación %XX UNA sola vez y rechazo de bytes peligrosos
 *           (NUL, caracteres de control, '\').
 *   Capa 3  Normalización por segmentos: se rechaza todo segmento ".." y todo
 *           segmento oculto (".git", ".htaccess"...). Ya no se busca la
 *           subcadena "..", así que "notas..v2.html" es un nombre válido.
 *   Capa 4  Confinamiento impuesto por el núcleo: openat2() con
 *           RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS
 *           relativo a un descriptor de la raíz web. Si el núcleo no tiene
 *           openat2 (Linux < 5.6) se usa realpath() + comparación de prefijo
 *           con '/' final + O_NOFOLLOW + verificación dev/inode.
 *   Capa 5  Se sirve el MISMO descriptor que se validó (fstat: sólo archivos
 *           regulares). No hay ventana TOCTOU entre comprobar y usar, y cada
 *           descriptor se cierra (el original filtraba 2 por petición).
 *   Capa 6  (si se ejecuta como root) chroot() a la raíz web y abandono de
 *           privilegios al usuario "nobody" antes de atender peticiones.
 *
 * Además se corrigen defectos de robustez que afectan la disponibilidad:
 * SIGPIPE ignorado, SO_REUSEADDR, timeout de lectura, procesos caídos que se
 * reemplazan, padre que ya no consume 100 % de CPU, mutex compartido entre
 * procesos con PTHREAD_PROCESS_SHARED y bitácora saneada (sin inyección).
 *
 * Compilar:  gcc -std=gnu11 -O2 -Wall -Wextra -pthread server_seguro.c -o server_seguro
 * Uso:       ./server_seguro [-p puerto] [-r raiz_web] [-b ip] [-w workers] [-n] [-x]
 *            -n  no hacer chroot/abandono de privilegios aunque se sea root
 *            -x  SÓLO DEMOSTRACIÓN: omite la capa 3 para comprobar que la capa 4 basta
 * ---------------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <time.h>
#ifdef SYS_openat2
#include <linux/openat2.h>
#endif

#define PUERTO_DEF     2001
#define WORKERS_DEF    10
#define MAX_PETICION   8192      /* bytes máximos de cabeceras            */
#define MAX_OBJETIVO   2048      /* longitud máxima de la ruta pedida     */
#define TIMEOUT_SEG    5         /* segundos para recibir las cabeceras   */

/* ------------------------------------------------------------------ */
/* Estado global                                                       */
/* ------------------------------------------------------------------ */
static int  g_listen = -1;               /* socket de escucha            */
static int  g_rootfd = -1;               /* descriptor de la raíz web     */
static char g_root[PATH_MAX];            /* ruta canónica de la raíz web  */
static int  g_chroot = 0;                /* 1 si se hizo chroot()         */
static int  g_demo_sin_capa3 = 0;        /* -x: SOLO para demostrar la capa 4 aislada */
static volatile sig_atomic_t g_salir = 0;

typedef struct {
    pthread_mutex_t lock;
    long long       total_bytes;
} compartido_t;
static compartido_t *g_shm;

/* Resultado de resolver una petición */
typedef struct {
    int         codigo;       /* código HTTP                            */
    int         fd;           /* archivo a enviar (-1 si página de error) */
    off_t       tam;          /* tamaño del archivo                     */
    const char *tipo;         /* Content-Type                           */
    const char *motivo;       /* explicación para la bitácora           */
} respuesta_t;

/* Páginas de error embebidas: no dependen del directorio de trabajo */
static const char *texto_estado(int c) {
    switch (c) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    default:  return "Internal Server Error";
    }
}

/* ------------------------------------------------------------------ */
/* Utilidades de E/S                                                   */
/* ------------------------------------------------------------------ */
static int enviar_todo(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Copia s en dst reemplazando bytes no imprimibles por '?' (evita inyección en la bitácora) */
static void sanear(char *dst, size_t cap, const char *s) {
    size_t i = 0;
    for (; s && *s && i + 1 < cap; s++)
        dst[i++] = (isprint((unsigned char)*s) ? *s : '?');
    dst[i] = '\0';
}

static const char *tipo_mime(const char *ruta) {
    const char *ext = strrchr(ruta, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".css"))  return "text/css";
    if (!strcasecmp(ext, ".js"))   return "application/javascript";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, ".ico"))  return "image/x-icon";
    if (!strcasecmp(ext, ".pdf"))  return "application/pdf";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* Capa 1: lectura y validación de la petición                         */
/* ------------------------------------------------------------------ */
/* Lee hasta el final de las cabeceras. Devuelve bytes leídos, -1 error/timeout, -2 demasiado grande */
static ssize_t leer_peticion(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;                 /* cierre o timeout (SO_RCVTIMEO) */
        n += (size_t)r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return (ssize_t)n;
    }
    return -2;
}

/* ------------------------------------------------------------------ */
/* Capa 2: decodificación %XX (una sola vez) y filtrado de bytes       */
/* ------------------------------------------------------------------ */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 0 = ok; -1 = codificación inválida o byte prohibido */
static int decodificar(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            int a = hexval(in[i + 1]), b = (a < 0) ? -1 : hexval(in[i + 2]);
            if (a < 0 || b < 0) return -1;
            c = (unsigned char)(a * 16 + b);
            i += 2;
        }
        if (c == '\0' || c < 0x20 || c == 0x7f || c == '\\') return -1;
        if (j + 1 >= cap) return -1;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capa 3: normalización por segmentos                                 */
/* ------------------------------------------------------------------ */
/* Convierte "/a//./b/" en "a/b/index.html". 0 ok, -1 segmento prohibido */
static int normalizar(const char *ruta, char *rel, size_t cap) {
    char copia[MAX_OBJETIVO + 1];
    snprintf(copia, sizeof copia, "%s", ruta);
    size_t len = strlen(copia);
    int termina_en_barra = (len > 0 && copia[len - 1] == '/');

    rel[0] = '\0';
    size_t usado = 0;
    char *guardar = NULL;
    for (char *seg = strtok_r(copia, "/", &guardar); seg; seg = strtok_r(NULL, "/", &guardar)) {
        if (strcmp(seg, ".") == 0) continue;            /* "." no aporta nada   */
        if (strcmp(seg, "..") == 0) return -1;          /* subir: prohibido     */
        if (seg[0] == '.') return -1;                    /* ocultos: prohibidos  */
        size_t sl = strlen(seg);
        if (usado + sl + 2 >= cap) return -1;
        if (usado) rel[usado++] = '/';
        memcpy(rel + usado, seg, sl);
        usado += sl;
        rel[usado] = '\0';
    }
    if (usado == 0) { snprintf(rel, cap, "index.html"); return 0; }
    if (termina_en_barra) {
        if (usado + 12 >= cap) return -1;
        strcat(rel, "/index.html");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Capa 4: apertura confinada                                          */
/* ------------------------------------------------------------------ */
/* Abre rel por debajo de dirfd. Devuelve fd o -errno. */
static int abrir_confinado(int dirfd, const char *rel) {
#ifdef SYS_openat2
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags   = O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK;  /* O_NONBLOCK: un FIFO no bloquea */
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    int fd = (int)syscall(SYS_openat2, dirfd, rel, &how, sizeof how);
    if (fd >= 0) return fd;
    if (errno != ENOSYS) return -errno;
#endif
    /* Respaldo para núcleos sin openat2: realpath + prefijo con '/' final. */
    char base[PATH_MAX], cand[PATH_MAX * 2], real[PATH_MAX];
    if (dirfd == g_rootfd) snprintf(base, sizeof base, "%s", g_chroot ? "" : g_root);
    else {
        char proc[64]; snprintf(proc, sizeof proc, "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(proc, base, sizeof base - 1);
        if (n < 0) return -EACCES;
        base[n] = '\0';
    }
    snprintf(cand, sizeof cand, "%s/%s", base, rel);
    if (!realpath(cand, real)) return -errno;
    const char *raiz = g_chroot ? "/" : g_root;
    size_t rl = strlen(raiz);
    if (strcmp(raiz, "/") != 0 &&
        !(strncmp(real, raiz, rl) == 0 && (real[rl] == '/' || real[rl] == '\0')))
        return -EXDEV;                                   /* fuera de la raíz */
    int fd2 = open(real, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW);
    if (fd2 < 0) return -errno;
    struct stat a, b;                                    /* ¿es el mismo inodo que validamos? */
    if (fstat(fd2, &a) < 0 || lstat(real, &b) < 0 || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
        close(fd2); return -EXDEV;
    }
    return fd2;
}

/* ------------------------------------------------------------------ */
/* Resolución completa de una petición                                 */
/* ------------------------------------------------------------------ */
static respuesta_t resolver(char *peticion, int *es_head, char *objetivo_log, size_t cap_log) {
    respuesta_t r = { 500, -1, 0, "text/html; charset=utf-8", "error interno" };
    *es_head = 0;
    objetivo_log[0] = '\0';

    /* Primera línea: MÉTODO SP OBJETIVO SP VERSIÓN */
    char *fin = strpbrk(peticion, "\r\n");
    if (!fin) { r.codigo = 400; r.motivo = "sin línea de petición"; return r; }
    *fin = '\0';
    sanear(objetivo_log, cap_log, peticion);

    char *sp1 = strchr(peticion, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || strchr(sp2 + 1, ' ')) { r.codigo = 400; r.motivo = "línea de petición mal formada"; return r; }
    *sp1 = '\0'; *sp2 = '\0';
    const char *metodo = peticion, *objetivo = sp1 + 1, *version = sp2 + 1;

    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) { r.codigo = 400; r.motivo = "versión HTTP no soportada"; return r; }
    if (!strcmp(metodo, "HEAD")) *es_head = 1;
    else if (strcmp(metodo, "GET")) { r.codigo = 405; r.motivo = "método no permitido"; return r; }
    if (strlen(objetivo) > MAX_OBJETIVO) { r.codigo = 414; r.motivo = "ruta demasiado larga"; return r; }
    if (objetivo[0] != '/') { r.codigo = 400; r.motivo = "la ruta no empieza por '/'"; return r; }

    /* Quitar cadena de consulta y fragmento */
    char ruta_cruda[MAX_OBJETIVO + 1];
    snprintf(ruta_cruda, sizeof ruta_cruda, "%s", objetivo);
    ruta_cruda[strcspn(ruta_cruda, "?#")] = '\0';

    /* Capa 2 */
    char ruta[MAX_OBJETIVO + 1];
    if (decodificar(ruta_cruda, ruta, sizeof ruta) < 0) { r.codigo = 400; r.motivo = "codificación inválida o byte prohibido"; return r; }

    /* Capa 3 */
    char rel[MAX_OBJETIVO + 32];
    if (g_demo_sin_capa3) {                    /* demostración: la ruta pasa tal cual a la capa 4 */
        const char *p = ruta; while (*p == '/') p++;
        snprintf(rel, sizeof rel, "%s", *p ? p : "index.html");
    } else if (normalizar(ruta, rel, sizeof rel) < 0) { r.codigo = 403; r.motivo = "segmento '..' u oculto rechazado"; return r; }

    /* Capa 4 */
    int fd = abrir_confinado(g_rootfd, rel);
    if (fd < 0) {
        int e = -fd;
        if (e == ENOENT || e == ENOTDIR) { r.codigo = 404; r.motivo = "no existe"; }
        else if (e == EXDEV)  { r.codigo = 403; r.motivo = "EXDEV: la resolución escaparía de la raíz"; }
        else if (e == ELOOP)  { r.codigo = 403; r.motivo = "ELOOP: enlace simbólico rechazado"; }
        else if (e == EACCES) { r.codigo = 403; r.motivo = "EACCES: sin permiso"; }
        else                  { r.codigo = 500; r.motivo = strerror(e); }
        return r;
    }

    /* Capa 5: se trabaja sobre el descriptor ya validado */
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); r.codigo = 500; r.motivo = "fstat"; return r; }
    if (S_ISDIR(st.st_mode)) {                 /* /docs -> /docs/index.html, confinado al directorio */
        int fd2 = abrir_confinado(fd, "index.html");
        close(fd);
        if (fd2 < 0) { r.codigo = 403; r.motivo = "directorio sin index.html (sin listados)"; return r; }
        fd = fd2;
        if (fstat(fd, &st) < 0) { close(fd); r.codigo = 500; r.motivo = "fstat"; return r; }
        strncat(rel, "/index.html", sizeof rel - strlen(rel) - 1);
    }
    if (!S_ISREG(st.st_mode)) { close(fd); r.codigo = 403; r.motivo = "no es un archivo regular"; return r; }
    int fl = fcntl(fd, F_GETFL);               /* volver a modo bloqueante para leer */
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    r.codigo = 200; r.fd = fd; r.tam = st.st_size; r.tipo = tipo_mime(rel); r.motivo = "servido";
    return r;
}

/* ------------------------------------------------------------------ */
/* Envío de la respuesta                                               */
/* ------------------------------------------------------------------ */
static long long responder(int cli, respuesta_t *r, int es_head) {
    char cuerpo_err[512];
    const char *cuerpo = NULL;
    size_t largo = (size_t)r->tam;

    if (r->fd < 0) {
        snprintf(cuerpo_err, sizeof cuerpo_err,
                 "<html><head><title>%d %s</title></head><body><h1>%d %s</h1></body></html>\n",
                 r->codigo, texto_estado(r->codigo), r->codigo, texto_estado(r->codigo));
        cuerpo = cuerpo_err;
        largo  = strlen(cuerpo_err);
        r->tipo = "text/html; charset=utf-8";
    }

    char cab[512];
    int n = snprintf(cab, sizeof cab,
                     "HTTP/1.0 %d %s\r\nServer: CS241Serv-seguro\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                     r->codigo, texto_estado(r->codigo), r->tipo, largo);
    long long total = 0;
    if (enviar_todo(cli, cab, (size_t)n) == 0) total += n;
    if (es_head) return total;

    if (cuerpo) {
        if (enviar_todo(cli, cuerpo, largo) == 0) total += (long long)largo;
    } else {
        char buf[16384];
        ssize_t k;
        while ((k = read(r->fd, buf, sizeof buf)) > 0) {
            if (enviar_todo(cli, buf, (size_t)k) < 0) break;
            total += k;
        }
    }
    return total;
}

static long long registrar_bytes(long long n) {
    pthread_mutex_lock(&g_shm->lock);
    g_shm->total_bytes += n;
    long long t = g_shm->total_bytes;
    pthread_mutex_unlock(&g_shm->lock);
    return t;
}

/* ------------------------------------------------------------------ */
/* Proceso trabajador                                                  */
/* ------------------------------------------------------------------ */
static void trabajador(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int cli = accept(g_listen, NULL, NULL);
        if (cli < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; perror("accept"); sleep(1); continue; }

        struct timeval tv = { TIMEOUT_SEG, 0 };
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        char buf[MAX_PETICION];
        char linea_log[160];
        int es_head = 0;
        respuesta_t r;
        ssize_t n = leer_peticion(cli, buf, sizeof buf);
        if (n == -1) { close(cli); continue; }                       /* timeout o cierre */
        if (n == -2) { r = (respuesta_t){ 431, -1, 0, "text/html", "cabeceras demasiado grandes" }; strcpy(linea_log, "(cabeceras > 8 KiB)"); }
        else r = resolver(buf, &es_head, linea_log, sizeof linea_log);

        long long enviados = responder(cli, &r, es_head);
        if (r.fd >= 0) close(r.fd);                                 /* sin fugas de descriptores */
        close(cli);

        long long total = registrar_bytes(enviados);
        printf("[pid %d] %d  \"%s\"  -> %s  (%lld bytes, total %lld)\n",
               getpid(), r.codigo, linea_log, r.motivo, enviados, total);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ */
/* Proceso padre                                                       */
/* ------------------------------------------------------------------ */
static void al_terminar(int sig) { (void)sig; g_salir = 1; }

static void uso(const char *p) {
    fprintf(stderr, "Uso: %s [-p puerto] [-r raiz_web] [-b ip] [-w workers] [-n] [-x]\n"
                    "  -n  no hacer chroot ni abandonar privilegios\n"
                    "  -x  DEMOSTRACIÓN: desactiva la capa 3 para probar la capa 4 aislada\n", p);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    int puerto = PUERTO_DEF, nworkers = WORKERS_DEF, sin_chroot = 0, opt;
    const char *raiz = "public_html", *ip = "0.0.0.0";
    while ((opt = getopt(argc, argv, "p:r:b:w:nxh")) != -1) {
        switch (opt) {
        case 'p': puerto = atoi(optarg); break;
        case 'r': raiz = optarg; break;
        case 'b': ip = optarg; break;
        case 'w': nworkers = atoi(optarg); break;
        case 'n': sin_chroot = 1; break;
        case 'x': g_demo_sin_capa3 = 1; break;
        default:  uso(argv[0]);
        }
    }
    if (puerto <= 0 || puerto > 65535 || nworkers <= 0 || nworkers > 64) uso(argv[0]);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_terminar;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Raíz web: ruta canónica absoluta + descriptor (no depende del CWD) */
    if (!realpath(raiz, g_root)) { perror("realpath(raiz)"); return EXIT_FAILURE; }
    g_rootfd = open(g_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (g_rootfd < 0) { perror("open(raiz)"); return EXIT_FAILURE; }

    /* Socket de escucha */
    g_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen < 0) { perror("socket"); return EXIT_FAILURE; }
    int uno = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    struct sockaddr_in dir; memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_port   = htons((unsigned short)puerto);
    if (inet_pton(AF_INET, ip, &dir.sin_addr) != 1) { fprintf(stderr, "IP inválida: %s\n", ip); return EXIT_FAILURE; }
    if (bind(g_listen, (struct sockaddr *)&dir, sizeof dir) < 0) { perror("bind"); return EXIT_FAILURE; }
    if (listen(g_listen, 64) < 0) { perror("listen"); return EXIT_FAILURE; }

    /* Memoria compartida anónima (sin nombre en /dev/shm) con mutex entre procesos */
    g_shm = mmap(NULL, sizeof *g_shm, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shm == MAP_FAILED) { perror("mmap"); return EXIT_FAILURE; }
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_setpshared(&at, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g_shm->lock, &at);
    g_shm->total_bytes = 0;

    /* Capa 6: si somos root, encerrarnos en la raíz web y soltar privilegios */
    if (geteuid() == 0 && !sin_chroot) {
        struct passwd *pw = getpwnam("nobody");
        if (!pw) { fprintf(stderr, "No existe el usuario nobody\n"); return EXIT_FAILURE; }
        if (chroot(g_root) < 0 || chdir("/") < 0) { perror("chroot"); return EXIT_FAILURE; }
        g_chroot = 1;
        if (setgroups(0, NULL) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            perror("abandono de privilegios"); return EXIT_FAILURE;
        }
        if (setuid(0) == 0) { fprintf(stderr, "Fallo: aún se puede recuperar root\n"); return EXIT_FAILURE; }
    }

    if (g_demo_sin_capa3)
        printf("AVISO: modo demostración -x, capa 3 desactivada (no usar en producción)\n");
    printf("Servidor seguro escuchando en %s:%d, raíz web %s%s, %d workers, uid %d\n",
           ip, puerto, g_root, g_chroot ? " (chroot)" : "", nworkers, (int)getuid());
    fflush(stdout);

    /* Pool de procesos: se crean N y se reemplaza cualquiera que muera */
    pid_t *hijos = calloc((size_t)nworkers, sizeof(pid_t));
    for (;;) {
        for (int i = 0; i < nworkers && !g_salir; i++) {
            if (hijos[i] > 0) continue;
            pid_t p = fork();
            if (p < 0) { perror("fork"); sleep(1); continue; }
            if (p == 0) { trabajador(); _exit(0); }
            hijos[i] = p;
        }
        if (g_salir) break;
        int st;
        pid_t muerto = wait(&st);                       /* el padre duerme: 0 % CPU */
        if (muerto < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < nworkers; i++)
            if (hijos[i] == muerto) {
                fprintf(stderr, "worker %d terminó (estado %d); se reemplaza\n", muerto, st);
                hijos[i] = 0;
            }
    }
    printf("Cerrando servidor...\n");
    for (int i = 0; i < nworkers; i++) if (hijos[i] > 0) kill(hijos[i], SIGTERM);
    while (wait(NULL) > 0) ;
    close(g_listen);
    return EXIT_SUCCESS;
}
