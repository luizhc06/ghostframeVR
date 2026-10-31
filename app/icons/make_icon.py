"""Gera icon.ico (fantasminha de óculos VR em pixel art) só com a stdlib.

Mesmo desenho do GHOST em ui/app.js. Rodar: python3 make_icon.py
"""
import struct
import zlib
from pathlib import Path

GHOST = [
    "................",
    ".....######.....",
    "...##########...",
    "..############..",
    ".##############.",
    ".#KKKKKKKKKKKK#.",
    ".#KwKKKKKKwKKK#.",
    ".#KKKKKKKKKKKK#.",
    ".##############.",
    ".##############.",
    ".##############.",
    ".##############.",
    ".##############.",
    ".##.###..###.##.",
    ".#...#....#...#.",
    "................",
]
CORES = {"#": (0, 240, 255, 255), "K": (2, 8, 12, 255), "w": (255, 255, 255, 255), ".": (0, 0, 0, 0)}


def png(tam):
    escala = tam // 16
    linhas = b""
    for y in range(tam):
        linha = GHOST[y // escala]
        linhas += b"\0" + b"".join(bytes(CORES[linha[x // escala]]) for x in range(tam))

    def bloco(tipo, dados):
        return struct.pack(">I", len(dados)) + tipo + dados + struct.pack(">I", zlib.crc32(tipo + dados))

    ihdr = struct.pack(">IIBBBBB", tam, tam, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + bloco(b"IHDR", ihdr) + bloco(b"IDAT", zlib.compress(linhas, 9)) + bloco(b"IEND", b"")


def ico(tamanhos):
    imagens = [png(t) for t in tamanhos]
    cab = struct.pack("<HHH", 0, 1, len(imagens))
    pos = 6 + 16 * len(imagens)
    dir_ = b""
    for t, img in zip(tamanhos, imagens):
        dir_ += struct.pack("<BBBBHHII", t % 256, t % 256, 0, 0, 1, 32, len(img), pos)
        pos += len(img)
    return cab + dir_ + b"".join(imagens)


if __name__ == "__main__":
    aqui = Path(__file__).parent
    (aqui / "icon.ico").write_bytes(ico([16, 32, 48, 256]))
    (aqui / "icon.png").write_bytes(png(256))
