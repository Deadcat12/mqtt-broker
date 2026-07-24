#!/usr/bin/env python3
import os
import signal
import socket
import struct
import subprocess
import time

HOST = '127.0.0.1'
PORT = 18883
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BROKER = os.path.join(ROOT, 'sol-broker')


def enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n:
            d |= 128
        out.append(d)
        if not n:
            return bytes(out)


def utf(s: str) -> bytes:
    b = s.encode()
    return struct.pack('!H', len(b)) + b


def connect_packet(client_id: str) -> bytes:
    vh = utf('MQTT') + bytes([4, 2]) + struct.pack('!H', 60)
    payload = utf(client_id)
    return bytes([0x10]) + enc_len(len(vh) + len(payload)) + vh + payload


def subscribe_packet(pid: int, topic_filter: str, qos: int = 0) -> bytes:
    payload = struct.pack('!H', pid) + utf(topic_filter) + bytes([qos])
    return bytes([0x82]) + enc_len(len(payload)) + payload


def publish_packet(topic: str, payload: bytes, qos: int = 0, pid: int = 1, retain: bool = False) -> bytes:
    body = utf(topic)
    fixed = 0x30 | (qos << 1) | (1 if retain else 0)
    if qos:
        body += struct.pack('!H', pid)
    body += payload
    return bytes([fixed]) + enc_len(len(body)) + body


def read_packet(sock: socket.socket) -> bytes:
    first = sock.recv(1)
    if not first:
        raise RuntimeError('socket closed')
    rl = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            raise RuntimeError('socket closed while reading remaining length')
        rl += b
        if not (b[0] & 128):
            break
    multiplier = 1
    total = 0
    for b in rl:
        total += (b & 127) * multiplier
        multiplier *= 128
    body = b''
    while len(body) < total:
        part = sock.recv(total - len(body))
        if not part:
            raise RuntimeError('socket closed while reading body')
        body += part
    return first + bytes(rl) + body


def main() -> None:
    proc = subprocess.Popen([BROKER, HOST, str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 2
        while True:
            try:
                s1 = socket.create_connection((HOST, PORT), timeout=0.2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)

        s2 = socket.create_connection((HOST, PORT), timeout=2)
        s1.sendall(connect_packet('subscriber'))
        assert read_packet(s1) == b'\x20\x02\x00\x00'
        s2.sendall(connect_packet('publisher'))
        assert read_packet(s2) == b'\x20\x02\x00\x00'

        s1.sendall(subscribe_packet(7, 'diploma/#', 0))
        suback = read_packet(s1)
        assert suback == b'\x90\x03\x00\x07\x00', suback

        s2.sendall(publish_packet('diploma/demo', b'hello', 0))
        incoming = read_packet(s1)
        assert incoming[0] == 0x30, incoming
        assert b'diploma/demo' in incoming and incoming.endswith(b'hello'), incoming

        s1.close(); s2.close()
        print('OK: CONNECT/SUBSCRIBE/PUBLISH tested')
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == '__main__':
    main()
