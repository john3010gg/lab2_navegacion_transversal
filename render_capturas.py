"""Convierte las salidas de terminal capturadas (fNN.txt) en imágenes PNG estilo terminal."""
import re, sys, os
from PIL import Image, ImageDraw, ImageFont

D = sys.argv[1]
S = 2                      # factor de escala para nitidez
FS, LH, PAD, BAR = 14*S, 20*S, 18*S, 30*S
font  = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", FS)
bold  = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", FS)
tfont = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13*S)
BG, FG, TITLE_BG = (30,30,40), (220,220,225), (52,52,64)
PROMPT, RED, GREEN, YEL, GREY = (120,180,250), (255,110,110), (120,220,140), (240,200,90), (140,140,155)

ROJO  = r"RESPALDO|root:x:0:0|<defunct>|killed by SIGPIPE|SIGPIPE \{|\(sin respuesta|sin respuesta\)|99\.[0-9] |\) = [89]$|\b(676|1276)\b|después: 0|Error opening|bucle activo|no hay close|sin respuesta: 295|^P1[56] |cuerpo vacío\)$|> GET /\.\./"
VERDE = r"\b403\b|EXDEV|ELOOP|nobody|después: 10|sin respuesta: 0|: +50$|RESOLVE_BENEATH|200 OK \(respondida|0 advertencias|registradas por strace: 0|se reemplaza|^P17 .*200|Petición legítima.*200 OK|^P1[2-5] .*400 "
AMARI = r"^P17 .*400|> GET /secreto"
TITULOS = {
 "f01": "Terminal — compilación y arranque del servidor ORIGINAL",
 "f02": "Terminal — peticiones legítimas al servidor ORIGINAL",
 "f03": "Terminal — batería de confinamiento contra el ORIGINAL (antes)",
 "f04": "Terminal — el filtro abre el archivo antes de rechazarlo (ORIGINAL)",
 "f05": "Terminal — fuga de descriptores y caída del servicio (ORIGINAL)",
 "f06": "Terminal — conexiones vacías y clientes lentos (ORIGINAL)",
 "f07": "Terminal — quién normaliza la ruta: el cliente, no el servidor",
 "f08": "Terminal — compilación y arranque del servidor SEGURO",
 "f09": "Terminal — batería de confinamiento contra el SEGURO (después)",
 "f10": "Terminal — bitácora del servidor SEGURO con el motivo de cada decisión",
 "f11": "Terminal — capa 4 aislada: el núcleo impide el escape",
 "f12": "Terminal — strace del servidor SEGURO",
 "f13": "Terminal — pruebas de disponibilidad sobre el SEGURO (después)",
}

def color(line, fid):
    if line.startswith("$"): return PROMPT, bold
    despues = fid in ("f08","f09","f10","f11","f12","f13")
    if re.search(AMARI, line): return YEL, font
    if not despues and re.search(ROJO, line): return RED, font
    if despues and re.search(VERDE, line): return GREEN, font
    if line.startswith(("---","==")): return GREY, font
    return FG, font

for fn in sorted(os.listdir(D)):
    if not re.fullmatch(r"f\d\d\.txt", fn): continue
    fid = fn[:3]
    lines = open(os.path.join(D, fn), encoding="utf-8").read().rstrip("\n").split("\n")
    cw = font.getlength("M")
    W = int(max(70, max(len(l) for l in lines)) * cw + 2*PAD)
    H = BAR + PAD + len(lines)*LH + PAD
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rectangle([0,0,W,BAR], fill=TITLE_BG)
    for i,c in enumerate([(255,95,87),(254,188,46),(40,200,64)]):
        cx = PAD + i*20*S; d.ellipse([cx, BAR//2-6*S, cx+12*S, BAR//2+6*S], fill=c)
    d.text((PAD + 70*S, BAR//2), TITULOS.get(fid, fid), font=tfont, fill=(200,200,210), anchor="lm")
    y = BAR + PAD
    for l in lines:
        col, f = color(l, fid)
        d.text((PAD, y), l, font=f, fill=col)
        y += LH
    im.save(os.path.join(D, fid + ".png"), optimize=True)
    print(fid, im.size)
