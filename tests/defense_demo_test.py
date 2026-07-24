#!/usr/bin/env python3
import os, signal, socket, struct, subprocess, time

HOST = "127.0.0.1"
PORT = int(os.environ.get("SOL_TEST_PORT", "18884"))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BROKER = os.path.join(ROOT, "sol-broker")

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

def connect_packet(client_id, level=4):
    variable_header = utf("MQTT") + bytes([level, 0x02]) + struct.pack("!H", 60)
    payload = utf(client_id)
    body = variable_header + payload
    return b"\x10" + enc_len(len(body)) + body

def subscribe_packet(packet_id, topic_filter, qos=0):
    body = struct.pack("!H", packet_id) + utf(topic_filter) + bytes([qos])
    return b"\x82" + enc_len(len(body)) + body

def unsubscribe_packet(packet_id, topic_filter):
    body = struct.pack("!H", packet_id) + utf(topic_filter)
    return b"\xA2" + enc_len(len(body)) + body

def publish_packet(topic, payload, qos=0, packet_id=1, retain=False):
    payload = payload if isinstance(payload, bytes) else payload.encode("utf-8")
    first_byte = 0x30 | ((qos & 3) << 1) | (0x01 if retain else 0x00)
    body = utf(topic)
    if qos:
        body += struct.pack("!H", packet_id)
    body += payload
    return bytes([first_byte]) + enc_len(len(body)) + body

def ack_packet(packet_type, packet_id):
    flags = 0x02 if packet_type == 6 else 0x00
    return bytes([(packet_type << 4) | flags, 0x02]) + struct.pack("!H", packet_id)

def read_packet(sock, timeout=2.0):
    sock.settimeout(timeout)
    first = sock.recv(1)
    if not first:
        raise RuntimeError("socket closed")
    rl = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            raise RuntimeError("socket closed while reading remaining length")
        rl += b
        if not (b[0] & 128):
            break
    multiplier = 1
    total = 0
    for b in rl:
        total += (b & 127) * multiplier
        multiplier *= 128
    body = b""
    while len(body) < total:
        part = sock.recv(total - len(body))
        if not part:
            raise RuntimeError("socket closed while reading body")
        body += part
    return first + bytes(rl) + body

def expect_no_packet(sock, delay=0.30):
    sock.setblocking(False)
    try:
        time.sleep(delay)
        try:
            data = sock.recv(1)
        except BlockingIOError:
            return
        raise AssertionError(f"unexpected data: {data!r}")
    finally:
        sock.setblocking(True)
        sock.settimeout(2)

def connect_client(client_id):
    s = socket.create_connection((HOST, PORT), timeout=2)
    s.sendall(connect_packet(client_id))
    got = read_packet(s)
    assert got == b"\x20\x02\x00\x00", f"bad CONNACK: {got!r}"
    return s

def close_client(sock):
    try:
        sock.sendall(b"\xE0\x00")
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass

def parse_publish(packet):
    assert packet[0] >> 4 == 3, f"not PUBLISH: {packet!r}"
    qos = (packet[0] >> 1) & 3
    retain = bool(packet[0] & 1)
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
    end = pos + remaining
    topic_len = struct.unpack("!H", packet[pos:pos+2])[0]
    pos += 2
    topic = packet[pos:pos+topic_len].decode("utf-8")
    pos += topic_len
    packet_id = None
    if qos:
        packet_id = struct.unpack("!H", packet[pos:pos+2])[0]
        pos += 2
    payload = packet[pos:end]
    return topic, payload, qos, retain, packet_id

def start_broker():
    if not os.path.exists(BROKER):
        raise SystemExit("sol-broker не найден. Сначала выполни: bash build.sh")
    proc = subprocess.Popen([BROKER, HOST, str(PORT)], cwd=ROOT,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 3
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SystemExit("Брокер сразу завершился. Вероятно, порт занят.")
        try:
            s = socket.create_connection((HOST, PORT), timeout=0.2)
            s.close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise SystemExit("Брокер не запустился за 3 секунды")

def stop_broker(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)

def test_connect_ping_disconnect():
    c = connect_client("defense-connect")
    c.sendall(b"\xC0\x00")
    assert read_packet(c) == b"\xD0\x00"
    close_client(c)

def test_subscribe_publish_qos0():
    sub = connect_client("defense-sub-qos0")
    pub = connect_client("defense-pub-qos0")
    sub.sendall(subscribe_packet(1, "demo/temperature", 0))
    assert read_packet(sub) == b"\x90\x03\x00\x01\x00"
    pub.sendall(publish_packet("demo/temperature", "23.5"))
    assert parse_publish(read_packet(sub))[:2] == ("demo/temperature", b"23.5")
    close_client(sub)
    close_client(pub)

def test_wildcards_plus_and_hash():
    plus = connect_client("defense-plus")
    hsh = connect_client("defense-hash")
    pub = connect_client("defense-wild-pub")
    plus.sendall(subscribe_packet(2, "factory/+/temp", 0))
    assert read_packet(plus) == b"\x90\x03\x00\x02\x00"
    hsh.sendall(subscribe_packet(3, "factory/#", 0))
    assert read_packet(hsh) == b"\x90\x03\x00\x03\x00"
    pub.sendall(publish_packet("factory/machine1/temp", "70"))
    assert parse_publish(read_packet(plus))[:2] == ("factory/machine1/temp", b"70")
    assert parse_publish(read_packet(hsh))[:2] == ("factory/machine1/temp", b"70")
    pub.sendall(publish_packet("factory/machine1/room/temp", "80"))
    expect_no_packet(plus)
    assert parse_publish(read_packet(hsh))[:2] == ("factory/machine1/room/temp", b"80")
    close_client(plus)
    close_client(hsh)
    close_client(pub)

def test_unsubscribe():
    sub = connect_client("defense-unsub-sub")
    pub = connect_client("defense-unsub-pub")
    sub.sendall(subscribe_packet(4, "demo/unsub", 0))
    assert read_packet(sub) == b"\x90\x03\x00\x04\x00"
    sub.sendall(unsubscribe_packet(5, "demo/unsub"))
    assert read_packet(sub) == b"\xB0\x02\x00\x05"
    pub.sendall(publish_packet("demo/unsub", "must-not-arrive"))
    expect_no_packet(sub)
    close_client(sub)
    close_client(pub)

def test_qos1_and_qos2():
    sub = connect_client("defense-qos-sub")
    pub = connect_client("defense-qos-pub")
    sub.sendall(subscribe_packet(6, "demo/qos", 1))
    assert read_packet(sub) == b"\x90\x03\x00\x06\x01"
    pub.sendall(publish_packet("demo/qos", "qos1-message", qos=1, packet_id=77))
    assert parse_publish(read_packet(sub))[0:3] == ("demo/qos", b"qos1-message", 1)
    assert read_packet(pub) == b"\x40\x02\x00\x4D"
    pub.sendall(publish_packet("demo/qos", "qos2-message", qos=2, packet_id=88))
    assert parse_publish(read_packet(sub))[0:2] == ("demo/qos", b"qos2-message")
    assert read_packet(pub) == b"\x50\x02\x00\x58"
    pub.sendall(ack_packet(6, 88))
    assert read_packet(pub) == b"\x70\x02\x00\x58"
    close_client(sub)
    close_client(pub)

TESTS = [
    ("CONNECT / CONNACK / PINGREQ / PINGRESP / DISCONNECT", test_connect_ping_disconnect),
    ("SUBSCRIBE / SUBACK / PUBLISH QoS0", test_subscribe_publish_qos0),
    ("Wildcard topics + and #", test_wildcards_plus_and_hash),
    ("UNSUBSCRIBE / UNSUBACK stops delivery", test_unsubscribe),
    ("QoS1 PUBACK and QoS2 PUBREC-PUBREL-PUBCOMP", test_qos1_and_qos2),
]

def main():
    proc = start_broker()
    try:
        for name, fn in TESTS:
            fn()
            print(f"[OK] {name}", flush=True)
    finally:
        stop_broker(proc)
    print(f"\nALL OK: {len(TESTS)} defense tests passed")

if __name__ == "__main__":
    main()
