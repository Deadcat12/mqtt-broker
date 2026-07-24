#!/usr/bin/env python3
import socket
import struct
import sys

HOST = "127.0.0.1"
PORT = 18884
CLIENT_ID = "own-demo-publisher"

topic = sys.argv[1] if len(sys.argv) > 1 else "demo/temperature"
message = sys.argv[2] if len(sys.argv) > 2 else "23.5"

def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 128
        out.append(b)
        if not n:
            return bytes(out)

def utf(s):
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b

def connect_packet(client_id):
    variable_header = utf("MQTT") + bytes([4, 0x02]) + struct.pack("!H", 60)
    payload = utf(client_id)
    body = variable_header + payload
    return b"\x10" + enc_len(len(body)) + body

def publish_packet(topic, payload):
    payload = payload.encode("utf-8")
    body = utf(topic) + payload
    return b"\x30" + enc_len(len(body)) + body

def read_packet(sock):
    first = sock.recv(1)
    if not first:
        raise RuntimeError("connection closed")

    remaining_bytes = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            raise RuntimeError("connection closed")
        remaining_bytes += b
        if not (b[0] & 128):
            break

    multiplier = 1
    remaining = 0
    for b in remaining_bytes:
        remaining += (b & 127) * multiplier
        multiplier *= 128

    body = b""
    while len(body) < remaining:
        body += sock.recv(remaining - len(body))

    return first + bytes(remaining_bytes) + body

s = socket.create_connection((HOST, PORT))

s.sendall(connect_packet(CLIENT_ID))
connack = read_packet(s)

if connack != b"\x20\x02\x00\x00":
    raise SystemExit(f"Ошибка CONNACK: {connack!r}")

print(f"[OK] CONNECT: client_id={CLIENT_ID}")

s.sendall(publish_packet(topic, message))
print(f"[OK] PUBLISH: {topic} = {message}")

s.sendall(b"\xE0\x00")
s.close()

print("[OK] DISCONNECT")
