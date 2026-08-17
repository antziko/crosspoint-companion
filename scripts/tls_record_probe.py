"""Transparent TCP relay that reports TLS record sizes sent by the server.

TLS record headers (type, version, 2-byte length) are NOT encrypted, so a relay
sitting in the TCP path can frame the stream without touching the crypto. The
client still does a normal end-to-end TLS handshake with the real origin.
"""
import socket
import sys
import threading

HOST = sys.argv[1]
PORT = 443
LISTEN = int(sys.argv[2])

TYPES = {20: "CCS", 21: "Alert", 22: "Handshake", 23: "AppData"}


def pump_plain(src, dst):
    try:
        while True:
            b = src.recv(65535)
            if not b:
                break
            dst.sendall(b)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def pump_framed(src, dst, out):
    buf = b""
    try:
        while True:
            b = src.recv(65535)
            if not b:
                break
            dst.sendall(b)
            buf += b
            while len(buf) >= 5:
                ctype = buf[0]
                ln = int.from_bytes(buf[3:5], "big")
                if len(buf) < 5 + ln:
                    break
                out.append((TYPES.get(ctype, str(ctype)), ln))
                buf = buf[5 + ln:]
    except OSError:
        pass


def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", LISTEN))
    srv.listen(1)
    cli, _ = srv.accept()
    up = socket.create_connection((HOST, PORT))
    records = []
    t = threading.Thread(target=pump_plain, args=(cli, up))
    t.start()
    pump_framed(up, cli, records)
    t.join(timeout=2)
    cli.close()
    up.close()

    app = [n for k, n in records if k == "AppData"]
    print(f"host={HOST}")
    print(f"records total={len(records)} appdata={len(app)}")
    if app:
        print(f"appdata bytes={sum(app)} max={max(app)} min={min(app)}")
        print("appdata record sizes in order:")
        print("  " + ", ".join(str(n) for n in app))


main()
