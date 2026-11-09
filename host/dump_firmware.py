#!/usr/bin/env python3
"""dump_firmware.py - Recibe el dump de la flash desde el ESP32-S3.

Uso:
    python3 dump_firmware.py [--port /dev/cu.usbmodemXXXX] [--size 4194304] [--out archivo.bin]

Flujo:
    1. Abre el puerto serie (el ESP32 se resetea solo al abrir, si hay puente USB-UART;
       con el USB nativo del ESP32-S3 no hay reset, por eso si no llega READY igual
       se envia la 'D' de todas formas).
    2. Espera la linea "READY" del firmware (con timeout corto).
    3. Manda 'D' y recibe exactamente `size` bytes crudos.
    4. Lee la linea "CRC32:xxxxxxxx" y la verifica localmente.
    5. Guarda <out>.bin + <out>.sha256.

Correrlo dos veces y comparar los SHA256 para validar el dump.
"""

import argparse
import hashlib
import sys
import time
import zlib

import serial
from serial.tools import list_ports

DEFAULT_SIZE = 4 * 1024 * 1024
READY_TIMEOUT = 5           # segundos esperando el boot del ESP32
DATA_TIMEOUT = 60           # segundos max sin recibir datos
PROGRESS_EVERY = 256 * 1024

# IDs conocidos: Espressif USB-Serial-JTAG nativo y puentes comunes
KNOWN_VID_PID = {
    (0x303A, 0x1001),  # ESP32-S3 USB-Serial-JTAG
    (0x303A, 0x0002),  # ESP32-S2/S3 variantes
    (0x1A86, 0x7523),  # CH340
    (0x1A86, 0x5523),  # CH341
    (0x10C4, 0xEA60),  # CP2102
}
PORT_HINTS = ("usbmodem", "usbserial", "wchusbserial", "SLAB_USBtoUART")


def find_port():
    ports = list(list_ports.comports())
    for p in ports:
        if (p.vid, p.pid) in KNOWN_VID_PID:
            return p.device
    for p in ports:
        if any(h in p.device for h in PORT_HINTS):
            return p.device
    if ports:
        print("No se detecto el ESP32 automaticamente. Puertos disponibles:", file=sys.stderr)
        for p in ports:
            print(f"  {p.device}  ({p.description}, vid={p.vid}, pid={p.pid})", file=sys.stderr)
    return None


def wait_ready(ser):
    deadline = time.time() + READY_TIMEOUT
    while time.time() < deadline:
        line = ser.readline()
        if b"READY" in line:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="Puerto serie (ej. /dev/cu.usbmodem1101)")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE,
                    help="Bytes a leer (default: 4 MB)")
    ap.add_argument("--out", help="Nombre del archivo de salida (default: dump_<ts>.bin)")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("Pasame el puerto con --port")
    print(f"Puerto: {port}")

    with serial.Serial(port, 115200, timeout=DATA_TIMEOUT) as ser:
        ser.reset_input_buffer()

        print("Esperando READY (si el puerto resetea al ESP32, tarda unos segundos)...")
        if not wait_ready(ser):
            # En algunas placas (p. ej. con chip puente CH340/CP2102) abrir el
            # puerto NO resetea el ESP32: el firmware ya impreso READY y esta
            # esperando el 'D'. Se lo mandamos igual; si arranca el dump, listo.
            print("No llego READY; intentando mandar 'D' de todas formas...")

        print("Enviando orden de dump...")
        ser.write(b"D")
        ser.flush()

        data = bytearray()
        start = time.time()
        while len(data) < args.size:
            chunk = ser.read(min(65536, args.size - len(data)))
            if not chunk:
                if not data:
                    sys.exit("No llego ningun byte. Verifica con 'idf.py monitor' que el "
                             "firmware este corriendo (deberia imprimir 'JEDEC ID: ...' "
                             "y 'READY'). Ojo: el monitor y este script no pueden usar el "
                             "puerto a la vez.")
                sys.exit(f"Timeout: recibidos {len(data)}/{args.size} bytes")
            data.extend(chunk)
            if len(data) % PROGRESS_EVERY < 65536:
                pct = 100 * len(data) / args.size
                print(f"  {len(data) // 1024} KB / {args.size // 1024} KB ({pct:.0f}%)")

        elapsed = time.time() - start
        kbps = args.size / elapsed / 1024
        print(f"Recibidos {len(data)} bytes en {elapsed:.1f}s ({kbps:.0f} KB/s)")

        # Linea CRC32 del firmware. El firmware la precede de un '\n' (para
        # separarla de los datos crudos), asi que hay que saltarse lineas
        # vacias hasta encontrarla.
        esp_crc = None
        line = ""
        for _ in range(4):
            line = ser.readline().decode(errors="replace").strip()
            if line.startswith("CRC32:"):
                esp_crc = int(line.split(":")[1], 16)
                break

        local_crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
        if esp_crc is not None:
            if esp_crc == local_crc:
                print(f"CRC32 OK: {local_crc:08X}")
            else:
                print(f"¡CRC32 DISTINTO! ESP32={esp_crc:08X} local={local_crc:08X} "
                      "-> datos corruptos en USB (o desincronizado)", file=sys.stderr)
        else:
            print(f"No llego la linea CRC32 (llego: {line!r}). CRC local: {local_crc:08X}",
                  file=sys.stderr)

        fname = args.out or f"dump_{int(time.time())}.bin"
        with open(fname, "wb") as f:
            f.write(data)
        sha = hashlib.sha256(bytes(data)).hexdigest()
        with open(fname + ".sha256", "w") as f:
            f.write(sha + "\n")
        print(f"Guardado: {fname}")
        print(f"SHA256:   {sha}")


if __name__ == "__main__":
    main()
