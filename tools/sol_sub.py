#!/usr/bin/env python3
import socket
import struct
import sys

HOST = "127.0.0.1"
PORT = 18884
CLIENT_ID = "own-demo-subscriber"

topic_filter = sys.argv[1] if len(sys.argv) > 1 else "demo/temperature"

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

def subscribe_packet(packet_id, topic):
    body = struct.pack("!H", packet_id) + utf(topic) + b"\x00"
    return b"\x82" + enc_len(len(body)) + body

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
        part = sock.recv(remaining - len(body))
        if not part:
            raise RuntimeError("connection closed")
        body += part

    return first + bytes(remaining_bytes) + body

def parse_publish(packet):
    pos = 1

    multiplier = 1
    remaining = 0
    while True:
        b = packet[pos]
        pos += 1
        remaining += (b & 127) * multiplier
        multiplier *= 128
        if not (b & 128):
            break

    topic_len = struct.unpack("!H", packet[pos:pos+2])[0]
    pos += 2

    topic = packet[pos:pos+topic_len].decode("utf-8")
    pos += topic_len

    payload = packet[pos:].decode("utf-8", errors="replace")
    return topic, payload

s = socket.create_connection((HOST, PORT))
s.sendall(connect_packet(CLIENT_ID))

connack = read_packet(s)
if connack != b"\x20\x02\x00\x00":
    raise SystemExit(f"Ошибка CONNACK: {connack!r}")

print(f"[OK] CONNECT: client_id={CLIENT_ID}")

s.sendall(subscribe_packet(1, topic_filter))
suback = read_packet(s)

if suback != b"\x90\x03\x00\x01\x00":
    raise SystemExit(f"Ошибка SUBACK: {suback!r}")

print(f"[OK] SUBSCRIBE: {topic_filter}")
print("[WAIT] Ожидание сообщений... Ctrl+C для выхода")

try:
    while True:
        packet = read_packet(s)
        packet_type = packet[0] >> 4

        if packet_type == 3:
            topic, payload = parse_publish(packet)
            print(f"[MESSAGE] {topic} = {payload}")
        else:
            print(f"[INFO] Получен пакет: {packet!r}")
except KeyboardInterrupt:
    s.sendall(b"\xE0\x00")
    s.close()
    print("\n[OK] DISCONNECT")
