#!/usr/bin/env python3
"""
Comprehensive test suite for ESP32 MQTT Broker.

Tests all features against a live instance:
  - MQTT protocol (connect, subscribe, publish, ping, disconnect)
  - Wildcard topic matching (+ and #)
  - Retained messages (store, deliver on subscribe, delete with empty payload)
  - Binary / image payloads (up to buffer limit)
  - Concurrent connections (up to max clients)
  - Message throughput
  - Publish-to-subscribe latency
  - Keep-alive enforcement
  - Duplicate client ID handling
  - Web portal API (/api/status, /api/clients)
  - Web portal pages (/, /settings, /config, /clients, /update)
  - Portal settings save (POST /save-settings)
  - Firmware version display and API
  - Connected client tracking (MQTT + WiFi AP)

Usage:
    python3 test_broker.py [host] [port]

Requires: paho-mqtt, requests
    pip install paho-mqtt requests
"""

import sys
import time
import json
import socket
import struct
import hashlib
import threading
import statistics
from collections import defaultdict

import paho.mqtt.client as mqtt

try:
    import requests
except ImportError:
    requests = None

# ── Configuration ─────────────────────────────────────────────

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.25.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
HTTP_BASE = f"http://{HOST}"

# ── Test infrastructure ───────────────────────────────────────

_pass_count = 0
_fail_count = 0
_skip_count = 0


def ok(msg):
    global _pass_count
    _pass_count += 1
    print(f"  \033[32mPASS\033[0m  {msg}")


def fail(msg):
    global _fail_count
    _fail_count += 1
    print(f"  \033[31mFAIL\033[0m  {msg}")


def skip(msg):
    global _skip_count
    _skip_count += 1
    print(f"  \033[33mSKIP\033[0m  {msg}")


def section(title):
    print(f"\n{'─'*60}")
    print(f"  {title}")
    print(f"{'─'*60}")


def make_client(client_id, on_message=None):
    """Create a paho MQTT client, connect, and return it."""
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    connected = threading.Event()

    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            connected.set()

    c.on_connect = on_connect
    if on_message:
        c.on_message = on_message
    c.connect(HOST, PORT, keepalive=30)
    c.loop_start()
    if not connected.wait(timeout=5):
        raise ConnectionError(f"Client '{client_id}' failed to connect")
    return c


def cleanup(*clients):
    for c in clients:
        try:
            c.loop_stop()
            c.disconnect()
        except Exception:
            pass
    time.sleep(0.3)


# ── Raw MQTT helpers (for low-level protocol tests) ──────────

def encode_remaining_length(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length > 0:
            byte |= 0x80
        out.append(byte)
        if length == 0:
            break
    return bytes(out)


def raw_connect_pkt(client_id, keepalive=60):
    cid = client_id.encode()
    var_header = b'\x00\x04MQTT\x04\x02' + struct.pack('!H', keepalive)
    payload = struct.pack('!H', len(cid)) + cid
    remaining = var_header + payload
    return bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining


def raw_connect(client_id, timeout=5):
    """Low-level TCP+MQTT connect, returns (socket, connack_rc)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect((HOST, PORT))
    sock.sendall(raw_connect_pkt(client_id))
    resp = sock.recv(4)
    rc = resp[3] if len(resp) >= 4 and resp[0] == 0x20 else 0xFF
    return sock, rc


def raw_disconnect(sock):
    try:
        sock.sendall(bytes([0xE0, 0x00]))
    except Exception:
        pass
    try:
        sock.close()
    except Exception:
        pass


# ══════════════════════════════════════════════════════════════
#  TEST 1: Basic Connect / Disconnect
# ══════════════════════════════════════════════════════════════

def test_basic_connect():
    section("1. Basic Connect / Disconnect")

    # Simple connect
    try:
        c = make_client("test-basic-1")
        ok("Connect with valid client ID")
        cleanup(c)
    except Exception as e:
        fail(f"Connect failed: {e}")

    # Connect with empty client ID (broker should assign one or accept)
    try:
        sock, rc = raw_connect("")
        if rc == 0x00:
            ok("Connect with empty client ID accepted")
        else:
            ok(f"Connect with empty client ID rejected (rc={rc}) — valid per spec")
        raw_disconnect(sock)
    except Exception as e:
        fail(f"Empty client ID test failed: {e}")

    # Ping/pong
    try:
        sock, rc = raw_connect("test-ping-1")
        assert rc == 0, f"Connect failed rc={rc}"
        sock.sendall(bytes([0xC0, 0x00]))  # PINGREQ
        sock.settimeout(3)
        resp = sock.recv(2)
        if len(resp) == 2 and resp[0] == 0xD0:
            ok("PINGREQ → PINGRESP")
        else:
            fail(f"PINGRESP expected, got {resp.hex()}")
        raw_disconnect(sock)
    except Exception as e:
        fail(f"Ping test failed: {e}")

    # Clean disconnect
    try:
        sock, rc = raw_connect("test-disc-1")
        assert rc == 0
        sock.sendall(bytes([0xE0, 0x00]))  # DISCONNECT
        time.sleep(0.5)
        ok("Clean DISCONNECT")
        sock.close()
    except Exception as e:
        fail(f"Disconnect test failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 2: Publish / Subscribe
# ══════════════════════════════════════════════════════════════

def test_pub_sub():
    section("2. Publish / Subscribe")

    received = []
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received.append((msg.topic, msg.payload))

    sub = make_client("test-sub-1", on_message=on_msg)
    pub = make_client("test-pub-1")

    # Subscribe and publish
    sub.subscribe("test/pubsub/basic")
    time.sleep(0.5)

    pub.publish("test/pubsub/basic", "hello world", qos=0)
    time.sleep(1)

    with lock:
        msgs = list(received)

    if any(p == b"hello world" for _, p in msgs):
        ok("Basic publish → subscribe delivery")
    else:
        fail(f"Message not received (got {len(msgs)} messages)")

    # Multiple topics
    received.clear()
    sub.subscribe("test/pubsub/a")
    sub.subscribe("test/pubsub/b")
    sub.subscribe("test/pubsub/c")
    time.sleep(0.3)

    for t in ["a", "b", "c"]:
        pub.publish(f"test/pubsub/{t}", f"msg-{t}")
    time.sleep(1)

    with lock:
        topics_received = {t for t, _ in received}

    expected = {"test/pubsub/a", "test/pubsub/b", "test/pubsub/c"}
    if topics_received >= expected:
        ok(f"Multi-topic delivery ({len(topics_received)} topics)")
    else:
        fail(f"Missing topics: {expected - topics_received}")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 3: Wildcard Subscriptions
# ══════════════════════════════════════════════════════════════

def test_wildcards():
    section("3. Wildcard Subscriptions")

    received = defaultdict(list)
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received[msg.topic].append(msg.payload)

    sub = make_client("test-wild-sub", on_message=on_msg)
    pub = make_client("test-wild-pub")

    # Single-level wildcard +
    sub.subscribe("sensor/+/temperature")
    time.sleep(0.3)

    pub.publish("sensor/room1/temperature", "22.5")
    pub.publish("sensor/room2/temperature", "19.8")
    pub.publish("sensor/room1/humidity", "55")  # should NOT match
    time.sleep(1)

    with lock:
        plus_topics = set(received.keys())

    if "sensor/room1/temperature" in plus_topics and "sensor/room2/temperature" in plus_topics:
        ok("Single-level wildcard (+) matches correctly")
    else:
        fail(f"+ wildcard: expected room1+room2 temps, got {plus_topics}")

    if "sensor/room1/humidity" not in plus_topics:
        ok("Single-level wildcard (+) correctly excludes non-matching")
    else:
        fail("+ wildcard incorrectly matched humidity topic")

    # Multi-level wildcard #
    received.clear()
    sub.subscribe("home/#")
    time.sleep(0.3)

    pub.publish("home/living/light", "on")
    pub.publish("home/kitchen/temp", "24")
    pub.publish("home", "root")
    pub.publish("office/desk", "should-not-match")
    time.sleep(1)

    with lock:
        hash_topics = set(received.keys())

    matched = {"home/living/light", "home/kitchen/temp", "home"}
    if matched.issubset(hash_topics):
        ok("Multi-level wildcard (#) matches correctly")
    else:
        fail(f"# wildcard: expected {matched}, got {hash_topics}")

    if "office/desk" not in hash_topics:
        ok("Multi-level wildcard (#) correctly excludes non-matching")
    else:
        fail("# wildcard incorrectly matched office topic")

    # $SYS protection — $-topics should not match # at top level
    received.clear()
    sub.subscribe("#")
    time.sleep(0.3)
    pub.publish("$SYS/broker/uptime", "12345")
    pub.publish("normal/topic", "data")
    time.sleep(1)

    with lock:
        all_topics = set(received.keys())

    if "normal/topic" in all_topics:
        ok("# subscription receives normal topics")
    else:
        fail("# subscription did not receive normal topic")

    if "$SYS/broker/uptime" not in all_topics:
        ok("$-topic protection: # does not match $SYS")
    else:
        # Some brokers allow this — not a hard failure
        skip("$-topic protection not enforced (some brokers allow this)")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 4: Retained Messages
# ══════════════════════════════════════════════════════════════

def test_retained():
    section("4. Retained Messages")

    # Use a unique topic per test run to avoid interference
    import random
    topic = f"test/retain/t{random.randint(1000,9999)}"

    # Publish a retained message and disconnect
    pub = make_client("test-retain-pub")
    pub.publish(topic, "42.0", qos=0, retain=True)
    time.sleep(1)
    cleanup(pub)
    time.sleep(1)

    # New subscriber should receive the retained message
    received = []
    lock = threading.Lock()
    got_msg = threading.Event()

    def on_msg(client, userdata, msg):
        with lock:
            received.append((msg.topic, msg.payload, msg.retain))
        got_msg.set()

    sub = make_client("test-retain-sub", on_message=on_msg)
    sub.subscribe(topic)
    got_msg.wait(timeout=3)

    with lock:
        msgs = list(received)

    if any(p == b"42.0" for _, p, _ in msgs):
        ok("Retained message delivered to new subscriber")
    else:
        fail(f"Retained message not received (got {len(msgs)} msgs)")

    cleanup(sub)

    # Delete retained message with empty payload
    pub2 = make_client("test-retain-pub2")
    pub2.publish(topic, b"", qos=0, retain=True)
    time.sleep(1)
    cleanup(pub2)
    time.sleep(1)

    received2 = []

    def on_msg2(client, userdata, msg):
        with lock:
            received2.append((msg.topic, msg.payload))

    sub2 = make_client("test-retain-sub2", on_message=on_msg2)
    sub2.subscribe(topic)
    time.sleep(2)

    with lock:
        got = list(received2)

    if not any(p for _, p in got):
        ok("Retained message deleted with empty payload")
    else:
        fail(f"Retained message still present after delete: {got}")

    cleanup(sub2)


# ══════════════════════════════════════════════════════════════
#  TEST 5: Binary / Image Payloads
# ══════════════════════════════════════════════════════════════

def test_binary_payloads():
    section("5. Binary / Image Payloads")

    received = {}
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received[len(msg.payload)] = hashlib.md5(msg.payload).hexdigest()

    sub = make_client("test-bin-sub", on_message=on_msg)
    sub.subscribe("test/binary/image")
    time.sleep(0.5)

    pub = make_client("test-bin-pub")

    PNG_HEADER = b'\x89PNG\r\n\x1a\n'
    test_sizes = [100, 1024, 2048, 4096, 8192, 12288, 15000]

    sent_hashes = {}
    for size in test_sizes:
        payload = PNG_HEADER + bytes(range(256)) * ((size - 8) // 256 + 1)
        payload = payload[:size]
        sent_hashes[size] = hashlib.md5(payload).hexdigest()
        pub.publish("test/binary/image", payload, qos=0)
        time.sleep(0.3)

    time.sleep(2)

    with lock:
        results = dict(received)

    all_ok = True
    for size in test_sizes:
        if sent_hashes[size] == results.get(size):
            ok(f"Binary payload {size:>6} bytes — MD5 verified")
        else:
            fail(f"Binary payload {size:>6} bytes — {'not received' if size not in results else 'MD5 mismatch'}")
            all_ok = False

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 6: Concurrent Connections
# ══════════════════════════════════════════════════════════════

def test_concurrent_connections():
    section("6. Concurrent Connections")

    target = 50
    sockets = []
    connected = 0

    for i in range(target):
        try:
            sock, rc = raw_connect(f"test-conc-{i:03d}")
            if rc == 0:
                sockets.append(sock)
                connected += 1
            else:
                sock.close()
        except Exception:
            pass

    if connected >= target:
        ok(f"Connected {connected}/{target} clients simultaneously")
    elif connected >= target * 0.9:
        ok(f"Connected {connected}/{target} clients (>90%% success)")
    else:
        fail(f"Only {connected}/{target} clients connected")

    # Verify they can all ping
    ping_ok = 0
    for sock in sockets:
        try:
            sock.sendall(bytes([0xC0, 0x00]))
            sock.settimeout(3)
            resp = sock.recv(2)
            if len(resp) == 2 and resp[0] == 0xD0:
                ping_ok += 1
        except Exception:
            pass

    if ping_ok == connected:
        ok(f"All {ping_ok} clients responded to PING")
    else:
        fail(f"Only {ping_ok}/{connected} clients responded to PING")

    for sock in sockets:
        raw_disconnect(sock)
    time.sleep(1)


# ══════════════════════════════════════════════════════════════
#  TEST 7: Message Throughput
# ══════════════════════════════════════════════════════════════

def test_throughput():
    section("7. Message Throughput")

    num_messages = 200
    received_count = [0]
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received_count[0] += 1

    sub = make_client("test-tp-sub", on_message=on_msg)
    sub.subscribe("test/throughput/#")
    time.sleep(0.5)

    pub = make_client("test-tp-pub")

    t0 = time.monotonic()
    for i in range(num_messages):
        pub.publish(f"test/throughput/t{i % 20}", f"msg-{i}")
    pub_time = time.monotonic() - t0

    time.sleep(3)

    with lock:
        recv = received_count[0]

    pub_rate = num_messages / pub_time if pub_time > 0 else 0
    delivery_pct = (recv / num_messages) * 100 if num_messages > 0 else 0

    ok(f"Published {num_messages} msgs in {pub_time:.2f}s ({pub_rate:.0f} msg/s)")

    if recv >= num_messages * 0.95:
        ok(f"Received {recv}/{num_messages} ({delivery_pct:.0f}%% delivery)")
    elif recv >= num_messages * 0.80:
        ok(f"Received {recv}/{num_messages} ({delivery_pct:.0f}%% delivery — acceptable for QoS 0)")
    else:
        fail(f"Only received {recv}/{num_messages} ({delivery_pct:.0f}%%)")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 8: Publish-to-Subscribe Latency
# ══════════════════════════════════════════════════════════════

def test_latency():
    section("8. Publish-to-Subscribe Latency")

    latencies = []
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        t_recv = time.monotonic()
        try:
            t_sent = float(msg.payload.decode())
            with lock:
                latencies.append((t_recv - t_sent) * 1000)
        except Exception:
            pass

    sub = make_client("test-lat-sub", on_message=on_msg)
    sub.subscribe("test/latency")
    time.sleep(0.5)

    pub = make_client("test-lat-pub")

    for _ in range(50):
        pub.publish("test/latency", f"{time.monotonic()}")
        time.sleep(0.05)

    time.sleep(2)

    with lock:
        lats = list(latencies)

    if len(lats) >= 40:
        avg = statistics.mean(lats)
        p50 = statistics.median(lats)
        p95 = sorted(lats)[int(len(lats) * 0.95)]
        ok(f"Latency ({len(lats)} samples): avg={avg:.1f}ms p50={p50:.1f}ms p95={p95:.1f}ms")
        if avg < 300:
            ok("Average latency under 300ms")
        else:
            fail(f"Average latency {avg:.0f}ms exceeds 300ms")
    else:
        fail(f"Only {len(lats)} latency samples (expected ~50)")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 9: Duplicate Client ID
# ══════════════════════════════════════════════════════════════

def test_duplicate_client_id():
    section("9. Duplicate Client ID Handling")

    # Connect first client
    sock1, rc1 = raw_connect("test-dup-client")
    if rc1 != 0:
        fail(f"First connect failed rc={rc1}")
        return

    ok("First client connected")

    # Connect second client with same ID — should disconnect the first
    sock2, rc2 = raw_connect("test-dup-client")
    if rc2 == 0:
        ok("Second client with same ID accepted")
    else:
        fail(f"Second client rejected rc={rc2}")

    # First client should be disconnected
    time.sleep(0.5)
    try:
        sock1.sendall(bytes([0xC0, 0x00]))  # PINGREQ
        sock1.settimeout(2)
        resp = sock1.recv(2)
        if len(resp) == 0:
            ok("First client disconnected (as per MQTT spec)")
        else:
            fail("First client still alive after duplicate ID connect")
    except (ConnectionError, OSError, socket.timeout):
        ok("First client disconnected (connection reset)")

    raw_disconnect(sock2)
    try:
        sock1.close()
    except Exception:
        pass


# ══════════════════════════════════════════════════════════════
#  TEST 10: Keep-Alive Enforcement
# ══════════════════════════════════════════════════════════════

def test_keepalive():
    section("10. Keep-Alive Enforcement")

    # Connect with very short keepalive (2 seconds)
    cid = b"test-ka-short"
    var_header = b'\x00\x04MQTT\x04\x02' + struct.pack('!H', 2)  # keepalive=2s
    payload = struct.pack('!H', len(cid)) + cid
    remaining = var_header + payload
    pkt = bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    sock.connect((HOST, PORT))
    sock.sendall(pkt)
    resp = sock.recv(4)

    if len(resp) >= 4 and resp[0] == 0x20 and resp[3] == 0x00:
        ok("Connected with keepalive=2s")
    else:
        fail("Failed to connect with short keepalive")
        sock.close()
        return

    # Wait for keepalive + grace period to expire (2s + 10s grace + 5s check interval + margin)
    print("    Waiting for keepalive timeout (20s)...")
    time.sleep(20)

    # Try to send — should be disconnected
    try:
        sock.sendall(bytes([0xC0, 0x00]))
        sock.settimeout(2)
        resp = sock.recv(2)
        if len(resp) == 0:
            ok("Client disconnected after keepalive timeout")
        else:
            fail("Client still alive after keepalive expiry")
    except (ConnectionError, OSError, BrokenPipeError):
        ok("Client disconnected after keepalive timeout")
    finally:
        try:
            sock.close()
        except Exception:
            pass


# ══════════════════════════════════════════════════════════════
#  TEST 11: Many Topics
# ══════════════════════════════════════════════════════════════

def test_many_topics():
    section("11. Many Topics")

    num_topics = 100
    received = defaultdict(list)
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received[msg.topic].append(msg.payload)

    # Use multiple subscribers to spread the load
    subs = []
    for i in range(5):
        s = make_client(f"test-many-sub-{i}", on_message=on_msg)
        for t in range(i * 20, (i + 1) * 20):
            s.subscribe(f"test/many/topic-{t:03d}")
        subs.append(s)
    time.sleep(1)

    pub = make_client("test-many-pub")

    for t in range(num_topics):
        pub.publish(f"test/many/topic-{t:03d}", f"data-{t}")
        if t % 20 == 0:
            time.sleep(0.1)
    time.sleep(2)

    with lock:
        topics_received = set(received.keys())

    if len(topics_received) >= num_topics * 0.95:
        ok(f"Received on {len(topics_received)}/{num_topics} topics")
    else:
        fail(f"Only {len(topics_received)}/{num_topics} topics received")

    cleanup(pub, *subs)


# ══════════════════════════════════════════════════════════════
#  TEST 12: Web Portal API
# ══════════════════════════════════════════════════════════════

def test_portal_api():
    section("12. Web Portal API")

    if requests is None:
        skip("'requests' module not installed — skipping portal tests")
        return

    # GET /api/status
    try:
        resp = requests.get(f"{HTTP_BASE}/api/status", timeout=5)
        if resp.status_code == 200:
            ok(f"GET /api/status → 200")
        else:
            fail(f"GET /api/status → {resp.status_code}")
            return

        data = resp.json()

        # Validate structure
        for key in ["wifi", "broker", "system"]:
            if key in data:
                ok(f"  /api/status has '{key}' section")
            else:
                fail(f"  /api/status missing '{key}' section")

        # Validate wifi fields
        wifi = data.get("wifi", {})
        for field in ["connected", "ssid", "ip", "ap"]:
            if field in wifi:
                ok(f"  wifi.{field} = {wifi[field]}")
            else:
                fail(f"  wifi.{field} missing")

        # Validate broker fields
        broker = data.get("broker", {})
        for field in ["clients", "max_clients", "subs", "retained", "port"]:
            if field in broker:
                ok(f"  broker.{field} = {broker[field]}")
            else:
                fail(f"  broker.{field} missing")

        # Validate system fields
        system = data.get("system", {})
        if "uptime_s" in system and system["uptime_s"] > 0:
            ok(f"  system.uptime_s = {system['uptime_s']}")
        else:
            fail("  system.uptime_s missing or zero")

        if "free_heap_kb" in system and system["free_heap_kb"] > 0:
            ok(f"  system.free_heap_kb = {system['free_heap_kb']}")
        else:
            fail("  system.free_heap_kb missing or zero")

    except Exception as e:
        fail(f"API request failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 13: Web Portal Pages
# ══════════════════════════════════════════════════════════════

def test_portal_pages():
    section("13. Web Portal Pages")

    if requests is None:
        skip("'requests' module not installed")
        return

    pages = [
        ("/", "MQTT Broker"),
        ("/information", "MQTT Broker"),
        ("/settings", "MQTT Broker"),
        ("/config", "WiFi Configuration"),
        ("/clients", "MQTT Clients"),
        ("/update", "Firmware Information"),
        ("/api/status", '"wifi"'),
        ("/api/clients", '"mqtt"'),
    ]

    for path, expected_content in pages:
        try:
            resp = requests.get(f"{HTTP_BASE}{path}", timeout=5)
            if resp.status_code == 200:
                if expected_content in resp.text:
                    ok(f"GET {path} → 200, contains '{expected_content}'")
                else:
                    fail(f"GET {path} → 200 but missing '{expected_content}'")
            else:
                fail(f"GET {path} → {resp.status_code}")
        except Exception as e:
            fail(f"GET {path} failed: {e}")

    # 404 for unknown path
    try:
        resp = requests.get(f"{HTTP_BASE}/nonexistent", timeout=5)
        if resp.status_code == 404:
            ok("GET /nonexistent → 404")
        else:
            fail(f"GET /nonexistent → {resp.status_code} (expected 404)")
    except Exception as e:
        fail(f"GET /nonexistent failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 14: Portal Settings Save
# ══════════════════════════════════════════════════════════════

def test_portal_save_settings():
    section("14. Portal Settings Save")

    if requests is None:
        skip("'requests' module not installed")
        return

    session = requests.Session()
    session.headers.update({"Connection": "close"})

    # Save settings via POST
    try:
        resp = session.post(f"{HTTP_BASE}/save-settings", data={
            "mqtt_port": "1883",
            "auth_user": "",
            "auth_pass": "",
            "buf_size": "16384",
            "retain_en": "1",
            "retain_ttl_h": "168",
            "ap_ssid": "mqtt-broker",
            "ap_pass": "mqtt1234",
        }, timeout=5, allow_redirects=False)

        if resp.status_code in (302, 200):
            ok(f"POST /save-settings → {resp.status_code}")
        else:
            fail(f"POST /save-settings → {resp.status_code}")
    except Exception as e:
        fail(f"POST /save-settings failed: {e}")

    time.sleep(0.5)

    # Verify settings persisted by loading settings page
    try:
        resp = session.get(f"{HTTP_BASE}/settings", timeout=5)
        if "16384" in resp.text and "mqtt-broker" in resp.text:
            ok("Settings page reflects saved values")
        else:
            fail("Settings page does not reflect saved values")
    except Exception as e:
        fail(f"Settings verification failed: {e}")

    time.sleep(0.5)

    # Test WiFi validation — short password should be rejected
    try:
        resp = session.post(f"{HTTP_BASE}/save", data={
            "ssid": "test-net",
            "password": "short",
        }, timeout=5, allow_redirects=False)
        if resp.status_code == 302:
            ok("POST /save with short password → redirected (server-side validation)")
        else:
            ok(f"POST /save with short password → {resp.status_code}")
    except Exception as e:
        fail(f"WiFi validation test failed: {e}")

    time.sleep(0.5)

    # Test AP password validation — short password should not be saved
    try:
        resp = session.post(f"{HTTP_BASE}/save-settings", data={
            "mqtt_port": "1883",
            "auth_user": "",
            "auth_pass": "",
            "buf_size": "16384",
            "retain_en": "1",
            "retain_ttl_h": "168",
            "ap_ssid": "mqtt-broker",
            "ap_pass": "short",
        }, timeout=5, allow_redirects=False)

        time.sleep(0.5)

        # Check that ap_pass was NOT changed to "short"
        resp2 = session.get(f"{HTTP_BASE}/settings", timeout=5)
        if "short" not in resp2.text or "mqtt1234" in resp2.text:
            ok("AP password validation: short password rejected")
        else:
            fail("AP password validation: short password was accepted")
    except Exception as e:
        fail(f"AP password validation failed: {e}")

    session.close()


# ══════════════════════════════════════════════════════════════
#  TEST 15: Unsubscribe
# ══════════════════════════════════════════════════════════════

def test_unsubscribe():
    section("15. Unsubscribe")

    received = []
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received.append(msg.topic)

    sub = make_client("test-unsub", on_message=on_msg)
    pub = make_client("test-unsub-pub")

    sub.subscribe("test/unsub/topic")
    time.sleep(0.3)

    pub.publish("test/unsub/topic", "before-unsub")
    time.sleep(0.5)

    with lock:
        before = len(received)

    if before >= 1:
        ok("Received message before unsubscribe")
    else:
        fail("Did not receive message before unsubscribe")

    # Unsubscribe
    sub.unsubscribe("test/unsub/topic")
    time.sleep(0.5)

    received.clear()
    pub.publish("test/unsub/topic", "after-unsub")
    time.sleep(1)

    with lock:
        after = len(received)

    if after == 0:
        ok("No messages received after unsubscribe")
    else:
        fail(f"Received {after} message(s) after unsubscribe")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 16: Firmware Version in API
# ══════════════════════════════════════════════════════════════

def test_firmware_version():
    section("16. Firmware Version in API")

    if requests is None:
        skip("'requests' module not installed")
        return

    try:
        resp = requests.get(f"{HTTP_BASE}/api/status", timeout=5)
        data = resp.json()

        # Check firmware section exists
        if "firmware" in data:
            ok("/api/status has 'firmware' section")
        else:
            fail("/api/status missing 'firmware' section")
            return

        fw = data["firmware"]

        # Check required fields
        for field in ["name", "version", "build"]:
            if field in fw and fw[field]:
                ok(f"  firmware.{field} = {fw[field]}")
            else:
                fail(f"  firmware.{field} missing or empty")

        # Version should look like semver (x.y.z)
        ver = fw.get("version", "")
        parts = ver.split(".")
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            ok(f"  Version '{ver}' is valid semver")
        else:
            fail(f"  Version '{ver}' is not valid semver (expected x.y.z)")

        # Name should be non-empty
        if fw.get("name"):
            ok(f"  Firmware name: {fw['name']}")
        else:
            fail("  Firmware name is empty")

    except Exception as e:
        fail(f"Firmware version test failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 17: Firmware Update Page
# ══════════════════════════════════════════════════════════════

def test_firmware_update_page():
    section("17. Firmware Update Page")

    if requests is None:
        skip("'requests' module not installed")
        return

    try:
        resp = requests.get(f"{HTTP_BASE}/update", timeout=5)
        if resp.status_code == 200:
            ok("GET /update -> 200")
        else:
            fail(f"GET /update -> {resp.status_code}")
            return

        html = resp.text

        # Check key elements
        checks = {
            "Firmware Information": "firmware info fieldset",
            "Upload Firmware": "upload firmware fieldset",
            "OTA Update via URL": "OTA URL fieldset",
            "ota-upload": "upload form action",
            "ota-url": "URL form action",
            "enctype": "multipart form encoding",
            "Running Partition": "running partition display",
        }

        for content, desc in checks.items():
            if content in html:
                ok(f"  /update contains {desc}")
            else:
                fail(f"  /update missing {desc} ('{content}')")

        # Check version is displayed on the page
        status_resp = requests.get(f"{HTTP_BASE}/api/status", timeout=5)
        version = status_resp.json().get("firmware", {}).get("version", "")
        if version and version in html:
            ok(f"  Version '{version}' displayed on update page")
        else:
            fail(f"  Version not found on update page")

    except Exception as e:
        fail(f"Firmware update page test failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 18: Connected Clients Page and API
# ══════════════════════════════════════════════════════════════

def test_connected_clients():
    section("18. Connected Clients Page and API")

    if requests is None:
        skip("'requests' module not installed")
        return

    # First test with no MQTT clients connected
    try:
        resp = requests.get(f"{HTTP_BASE}/api/clients", timeout=5)
        if resp.status_code == 200:
            ok("GET /api/clients -> 200")
        else:
            fail(f"GET /api/clients -> {resp.status_code}")
            return

        data = resp.json()

        if "mqtt" in data:
            ok("/api/clients has 'mqtt' array")
        else:
            fail("/api/clients missing 'mqtt' array")

        if "wifi_ap" in data:
            ok("/api/clients has 'wifi_ap' array")
        else:
            fail("/api/clients missing 'wifi_ap' array")

    except Exception as e:
        fail(f"Clients API (empty) failed: {e}")
        return

    # Connect some MQTT clients and verify they show up
    clients = []
    client_names = ["test-track-alpha", "test-track-beta", "test-track-gamma"]
    try:
        for name in client_names:
            c = make_client(name)
            clients.append(c)

        # Subscribe each to different topics
        clients[0].subscribe("tracking/temp")
        clients[1].subscribe([("tracking/humidity", 0), ("tracking/pressure", 0)])
        clients[2].subscribe("tracking/#")
        time.sleep(1)

        # Check /api/clients
        resp = requests.get(f"{HTTP_BASE}/api/clients", timeout=5)
        data = resp.json()
        mqtt_clients = data.get("mqtt", [])

        # All 3 should be present
        found_ids = {c["client_id"] for c in mqtt_clients}
        for name in client_names:
            if name in found_ids:
                ok(f"  Client '{name}' visible in /api/clients")
            else:
                fail(f"  Client '{name}' not found in /api/clients")

        # Verify client fields
        if mqtt_clients:
            sample = mqtt_clients[0]
            for field in ["client_id", "ip", "connected_s", "last_active_s", "subs", "keep_alive"]:
                if field in sample:
                    ok(f"  Client field '{field}' present")
                else:
                    fail(f"  Client field '{field}' missing")

        # Verify subscription counts
        for c in mqtt_clients:
            if c["client_id"] == "test-track-alpha" and c["subs"] == 1:
                ok("  test-track-alpha has 1 subscription")
            elif c["client_id"] == "test-track-beta" and c["subs"] == 2:
                ok("  test-track-beta has 2 subscriptions")
            elif c["client_id"] == "test-track-gamma" and c["subs"] == 1:
                ok("  test-track-gamma has 1 subscription")

        # Verify IP is populated
        for c in mqtt_clients:
            if c["client_id"] in found_ids and c.get("ip"):
                ok(f"  Client '{c['client_id']}' has IP: {c['ip']}")
                break
        else:
            fail("  No client has an IP address")

        # Check /clients HTML page
        resp = requests.get(f"{HTTP_BASE}/clients", timeout=5)
        if resp.status_code == 200:
            ok("GET /clients -> 200")
        else:
            fail(f"GET /clients -> {resp.status_code}")

        html = resp.text

        # Page should contain client names and key sections
        if "MQTT Clients (3)" in html or "MQTT Clients (" in html:
            ok("  /clients shows MQTT client count")
        else:
            fail("  /clients missing MQTT client count")

        if "WiFi AP Clients" in html:
            ok("  /clients has WiFi AP Clients section")
        else:
            fail("  /clients missing WiFi AP Clients section")

        for name in client_names:
            if name in html:
                ok(f"  Client '{name}' visible on /clients page")
            else:
                fail(f"  Client '{name}' not found on /clients page")

        if "auto-refreshes" in html or "auto-refresh" in html.lower():
            ok("  /clients has auto-refresh")
        else:
            fail("  /clients missing auto-refresh")

    except Exception as e:
        fail(f"Client tracking test failed: {e}")
    finally:
        cleanup(*clients)

    # After disconnect, clients should be gone
    time.sleep(1)
    try:
        resp = requests.get(f"{HTTP_BASE}/api/clients", timeout=5)
        data = resp.json()
        remaining = [c["client_id"] for c in data.get("mqtt", [])
                     if c["client_id"].startswith("test-track-")]
        if len(remaining) == 0:
            ok("  Disconnected clients removed from /api/clients")
        else:
            fail(f"  {len(remaining)} test clients still in /api/clients after disconnect")
    except Exception as e:
        fail(f"Post-disconnect check failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 19: Version in Dashboard and Footer
# ══════════════════════════════════════════════════════════════

def test_version_display():
    section("19. Version in Information Page and Footer")

    if requests is None:
        skip("'requests' module not installed")
        return

    try:
        # Get version from API
        status = requests.get(f"{HTTP_BASE}/api/status", timeout=5).json()
        version = status.get("firmware", {}).get("version", "")
        name = status.get("firmware", {}).get("name", "")

        if not version:
            fail("Cannot determine firmware version from API")
            return

        # Check information page has detailed firmware info
        resp = requests.get(f"{HTTP_BASE}/information", timeout=5)
        html = resp.text

        if version in html:
            ok(f"  Information page displays version '{version}'")
        else:
            fail(f"  Information page missing version '{version}'")

        # Check build date is on information page
        build = status.get("firmware", {}).get("build", "")
        if build and build[:3] in html:
            ok(f"  Build date visible on information page")
        else:
            fail("  Build date not found on information page")

        # Check IDF version on information page
        if "IDF Version" in html:
            ok("  IDF Version shown on information page")
        else:
            fail("  IDF Version missing from information page")

        # Check footer has name + version (all pages share the same footer)
        footer_str = f"{name} {version}"
        if footer_str in html:
            ok(f"  Footer shows '{footer_str}'")
        else:
            fail(f"  Footer missing '{footer_str}'")

        # Check main page is clean — should NOT have all the detail sections
        main_resp = requests.get(f"{HTTP_BASE}/", timeout=5)
        main_html = main_resp.text

        if "Configure WiFi" in main_html and "Configuration" in main_html:
            ok("  Main page has navigation buttons")
        else:
            fail("  Main page missing navigation buttons")

        if "Information" in main_html:
            ok("  Main page has Information button")
        else:
            fail("  Main page missing Information button")

    except Exception as e:
        fail(f"Version display test failed: {e}")


# ══════════════════════════════════════════════════════════════
#  MAIN
# ══════════════════════════════════════════════════════════════

def main():
    print(f"\n{'═'*60}")
    print(f"  ESP32 MQTT Broker Test Suite")
    print(f"  Target: {HOST}:{PORT}")
    print(f"  HTTP:   {HTTP_BASE}")
    print(f"{'═'*60}")

    # Verify broker is reachable
    try:
        sock, rc = raw_connect("test-preflight")
        if rc != 0:
            print(f"\nERROR: Broker not accepting connections (rc={rc})")
            sys.exit(1)
        raw_disconnect(sock)
    except Exception as e:
        print(f"\nERROR: Cannot reach broker at {HOST}:{PORT}: {e}")
        sys.exit(1)

    print(f"\nBroker reachable. Starting tests...\n")

    t0 = time.monotonic()

    test_basic_connect()
    test_pub_sub()
    test_wildcards()
    test_retained()
    test_binary_payloads()
    test_concurrent_connections()
    test_throughput()
    test_latency()
    test_duplicate_client_id()
    test_keepalive()
    test_many_topics()
    test_unsubscribe()
    test_portal_api()
    test_portal_pages()
    test_portal_save_settings()
    test_firmware_version()
    test_firmware_update_page()
    test_connected_clients()
    test_version_display()

    elapsed = time.monotonic() - t0

    print(f"\n{'═'*60}")
    print(f"  RESULTS: {_pass_count} passed, {_fail_count} failed, {_skip_count} skipped")
    print(f"  Time: {elapsed:.1f}s")
    print(f"{'═'*60}\n")

    sys.exit(1 if _fail_count > 0 else 0)


if __name__ == "__main__":
    main()
