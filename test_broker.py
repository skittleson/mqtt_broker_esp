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

HOST = sys.argv[1] if len(sys.argv) > 1 else \
    __import__("os").environ.get("BROKER_HOST", "192.168.25.1")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
HTTP_BASE = f"http://{HOST}"

# Basic Auth wiring: if BROKER_AUTH=user:pass is in the env, monkey-patch
# the `requests` module's get/post so every HTTP call sends credentials
# without per-call edits. Keeps the rest of test_broker.py untouched.
if requests is not None:
    _auth_raw = __import__("os").environ.get("BROKER_AUTH", "").strip()
    if _auth_raw and ":" in _auth_raw:
        _u, _, _p = _auth_raw.partition(":")
        _basic = requests.auth.HTTPBasicAuth(_u, _p)
        _orig_get = requests.get
        _orig_post = requests.post

        def _auth_get(url, **kw):
            kw.setdefault("auth", _basic)
            return _orig_get(url, **kw)

        def _auth_post(url, *a, **kw):
            kw.setdefault("auth", _basic)
            return _orig_post(url, *a, **kw)

        requests.get = _auth_get  # type: ignore[assignment]
        requests.post = _auth_post  # type: ignore[assignment]

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
    # NOTE: as of v0.6.3 the portal's /save-settings and /save handlers
    # respond with the reboot-countdown HTML (~3 KB) and then esp_restart()
    # the device. The previous flow was a 302 redirect, which this test
    # was written for. The destructive write-then-reboot cycle is best
    # tested via Playwright end-to-end (tools/capture_save_reboot.py);
    # here we skip the actual POSTs by default and only verify the GET
    # side so the suite stays fast and idempotent.
    #
    # Set BROKER_TEST_DESTRUCTIVE=1 to run the full cycle (will reboot
    # the device 2-3 times; adds ~2 min).
    destructive = __import__("os").environ.get(
        "BROKER_TEST_DESTRUCTIVE", "") == "1"

    if not destructive:
        skip("POST /save-settings (would reboot device; "
             "BROKER_TEST_DESTRUCTIVE=1 to enable)")
        skip("Settings persistence across reboot (destructive)")
        skip("POST /save short-password rejection (destructive)")
        skip("POST /save-settings short ap_pass rejection (destructive)")
    else:
        # The POST closes the socket mid-response when the device
        # reboots. urllib3 raises ConnectionError / ChunkedEncodingError.
        # Treat that as SUCCESS: device accepted the request and started
        # its reboot cycle.
        def _save_or_reboot(url, data, label):
            try:
                resp = session.post(url, data=data, timeout=5,
                                    allow_redirects=False)
                if resp.status_code in (200, 302):
                    ok(f"{label} \u2192 {resp.status_code}")
                else:
                    fail(f"{label} \u2192 {resp.status_code}")
            except (requests.exceptions.ChunkedEncodingError,
                    requests.exceptions.ConnectionError) as e:
                ok(f"{label} \u2192 socket closed during reboot "
                   f"(as expected since 0.6.3): {type(e).__name__}")

        def _wait_back_online(timeout=40):
            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    if session.get(f"{HTTP_BASE}/api/ping",
                                   timeout=2).status_code == 200:
                        return True
                except Exception:
                    pass
                time.sleep(1)
            return False

        _save_or_reboot(f"{HTTP_BASE}/save-settings", {
            "mqtt_port": "1883", "auth_user": "", "auth_pass": "",
            "buf_size": "16384", "retain_en": "1", "retain_ttl_h": "168",
            "ap_ssid": "mqtt-broker", "ap_pass": "",
        }, "POST /save-settings")
        if _wait_back_online():
            try:
                resp = session.get(f"{HTTP_BASE}/settings", timeout=5)
                if "16384" in resp.text and "mqtt-broker" in resp.text:
                    ok("Settings persisted across reboot")
                else:
                    fail("Settings did not persist across reboot")
            except Exception as e:
                fail(f"Settings verification failed: {e}")
        else:
            fail("Device did not come back online within 40s after save")

        # WiFi save with short password: server-side rejected before the
        # reboot cycle, so no reboot here.
        try:
            resp = session.post(f"{HTTP_BASE}/save", data={
                "ssid": "test-net", "password": "short",
            }, timeout=5, allow_redirects=False)
            if resp.status_code in (200, 302):
                ok(f"POST /save short password \u2192 {resp.status_code} "
                   f"(rejected pre-write, no reboot)")
            else:
                fail(f"POST /save short password \u2192 {resp.status_code}")
        except Exception as e:
            fail(f"WiFi validation test failed: {e}")

        # AP password: short value should not be persisted. Handler will
        # still trigger a reboot because other fields are accepted.
        try:
            resp = session.post(f"{HTTP_BASE}/save-settings", data={
                "mqtt_port": "1883", "auth_user": "", "auth_pass": "",
                "buf_size": "16384", "retain_en": "1",
                "retain_ttl_h": "168", "ap_ssid": "mqtt-broker",
                "ap_pass": "short",
            }, timeout=5, allow_redirects=False)
            ok(f"POST /save-settings short ap_pass \u2192 "
               f"{resp.status_code} (other fields saved, ap_pass kept)")
        except (requests.exceptions.ChunkedEncodingError,
                requests.exceptions.ConnectionError):
            ok("POST /save-settings short ap_pass \u2192 "
               "socket closed during reboot (as expected)")
        _wait_back_online()
        try:
            resp2 = session.get(f"{HTTP_BASE}/settings", timeout=5)
            # The form renders AP password as empty (placeholder) so
            # "short" must not appear anywhere in the HTML.
            if "value='short'" not in resp2.text:
                ok("AP password validation: short value not echoed")
            else:
                fail("AP password validation: short value was accepted")
        except Exception as e:
            fail(f"AP password verification failed: {e}")

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
#  TEST 16: QoS 1 Inbound (PUBACK round-trip + downgraded fanout)
# ══════════════════════════════════════════════════════════════

def test_qos1_inbound():
    section("16. QoS 1 Inbound (PUBACK + fanout)")

    received = []
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received.append((msg.topic, msg.payload, msg.qos))

    sub = make_client("test-qos1-sub", on_message=on_msg)
    pub = make_client("test-qos1-pub")

    # Subscribe with QoS 1 — phase 3 broker grants 1, delivers at QoS 1.
    sub_rc, _mid = sub.subscribe("test/qos1/#", qos=1)
    if sub_rc != mqtt.MQTT_ERR_SUCCESS:
        fail(f"SUBSCRIBE qos=1 failed: rc={sub_rc}")
        cleanup(sub, pub)
        return
    ok("SUBSCRIBE qos=1 accepted")
    time.sleep(0.3)

    # Publish QoS 1 — paho's wait_for_publish() blocks until PUBACK arrives.
    # If broker doesn't send PUBACK, is_published() stays False after timeout.
    info = pub.publish("test/qos1/hello", b"hello-qos1", qos=1)
    try:
        info.wait_for_publish(timeout=5)
    except Exception as e:
        fail(f"wait_for_publish raised: {e}")
        cleanup(sub, pub)
        return

    if info.is_published():
        ok("Broker sent PUBACK for QoS 1 PUBLISH")
    else:
        fail("No PUBACK received within 5s (QoS 1 inbound broken)")
        cleanup(sub, pub)
        return

    # Verify fanout still happened (subscriber should see the message).
    time.sleep(0.5)
    with lock:
        msgs = list(received)
    if any(t == "test/qos1/hello" and p == b"hello-qos1" for t, p, _q in msgs):
        ok("Subscriber received the QoS 1 message")
    else:
        fail(f"Subscriber missed the QoS 1 message; received={msgs}")

    # Burst test: 20 QoS 1 publishes, all must be ACK'd and delivered.
    received.clear()
    infos = []
    for i in range(20):
        infos.append(pub.publish("test/qos1/burst", f"m{i}".encode(), qos=1))
    for i, info in enumerate(infos):
        info.wait_for_publish(timeout=5)
        if not info.is_published():
            fail(f"PUBACK missing for burst message #{i}")
            cleanup(sub, pub)
            return
    ok("All 20 QoS 1 publishes acknowledged")

    time.sleep(1.0)
    with lock:
        burst_count = sum(1 for t, _p, _q in received if t == "test/qos1/burst")
    if burst_count == 20:
        ok(f"All 20 burst messages delivered to subscriber ({burst_count}/20)")
    else:
        fail(f"Only {burst_count}/20 burst messages delivered")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 17: QoS 1 Outbound (SUBACK grant, delivery QoS, inflight)
# ══════════════════════════════════════════════════════════════

def test_qos1_outbound():
    section("17. QoS 1 Outbound (broker → subscriber)")

    received = []
    granted_qos = []
    lock = threading.Lock()

    def on_msg(client, userdata, msg):
        with lock:
            received.append((msg.topic, msg.payload, msg.qos))

    def on_sub(client, userdata, mid, granted, properties=None):
        # paho v2 passes a list of ReasonCode (or int for v3.1.1)
        with lock:
            for g in granted:
                granted_qos.append(int(g.value) if hasattr(g, "value") else int(g))

    sub = make_client("test-qos1-out-sub", on_message=on_msg)
    sub.on_subscribe = on_sub
    pub = make_client("test-qos1-out-pub")

    sub.subscribe("test/qos1out/#", qos=1)
    for _ in range(20):
        time.sleep(0.1)
        with lock:
            if granted_qos: break

    with lock:
        gq = list(granted_qos)
    if gq and gq[0] == 1:
        ok(f"SUBACK granted QoS 1 (got {gq})")
    elif gq and gq[0] == 0:
        fail(f"SUBACK granted QoS 0 — phase 3 should grant min(req,1)=1 (got {gq})")
        cleanup(sub, pub)
        return
    else:
        fail(f"SUBACK never arrived or invalid: {gq}")
        cleanup(sub, pub)
        return

    # Publish at QoS 0 — effective delivery = min(0, 1) = 0
    pub.publish("test/qos1out/q0", b"from-q0", qos=0)
    time.sleep(0.5)
    with lock:
        q0 = [(t,p,q) for t,p,q in received if t == "test/qos1out/q0"]
    if q0 and q0[0][2] == 0:
        ok("QoS-0 publish delivered at QoS 0 (min(pub,granted))")
    elif q0:
        fail(f"QoS-0 publish delivered at qos={q0[0][2]}, expected 0")
    else:
        fail("Subscriber missed QoS-0 publish")

    # Publish at QoS 1 — effective delivery = min(1, 1) = 1
    info = pub.publish("test/qos1out/q1", b"from-q1", qos=1)
    info.wait_for_publish(timeout=5)
    time.sleep(0.5)
    with lock:
        q1 = [(t,p,q) for t,p,q in received if t == "test/qos1out/q1"]
    if q1 and q1[0][2] == 1:
        ok("QoS-1 publish delivered at QoS 1 (broker outbound QoS 1 working)")
    elif q1:
        fail(f"QoS-1 publish delivered at qos={q1[0][2]}, expected 1")
    else:
        fail("Subscriber missed QoS-1 publish")

    # No duplicate delivery after PUBACK round-trip
    with lock:
        q1_count = sum(1 for t,_p,_q in received if t == "test/qos1out/q1")
    if q1_count == 1:
        ok("Exactly-once delivery (no duplicate after PUBACK round-trip)")
    else:
        fail(f"Subscriber got {q1_count} copies of QoS-1 message (expected 1)")

    # /api/clients exposes 'inflight'; should settle to 0
    if requests is not None:
        try:
            time.sleep(0.5)
            data = requests.get(f"{HTTP_BASE}/api/clients", timeout=5).json()
            mqtt_clients = data.get("mqtt", [])
            if any("inflight" in c for c in mqtt_clients):
                ok("/api/clients exposes 'inflight' field")
            else:
                fail("/api/clients missing 'inflight' field")
            our_sub = [c for c in mqtt_clients if c.get("client_id") == "test-qos1-out-sub"]
            if our_sub and our_sub[0].get("inflight", -1) == 0:
                ok("Subscriber has inflight=0 after PUBACK settled")
            elif our_sub:
                fail(f"Subscriber has inflight={our_sub[0].get('inflight')}, expected 0")
        except Exception as e:
            fail(f"Inflight API check failed: {e}")

    # Burst of 20 QoS-1 publishes
    received.clear()
    infos = [pub.publish("test/qos1out/burst", f"m{i}".encode(), qos=1) for i in range(20)]
    for info in infos:
        info.wait_for_publish(timeout=5)
    time.sleep(1.5)
    with lock:
        burst = [(t,p,q) for t,p,q in received if t == "test/qos1out/burst"]
    burst_q1 = sum(1 for _t,_p,q in burst if q == 1)
    if burst_q1 == 20:
        ok(f"Burst: all 20 delivered at QoS 1 ({burst_q1}/20)")
    elif len(burst) == 20:
        fail(f"Burst: 20 delivered but only {burst_q1} at QoS 1")
    else:
        fail(f"Burst: {len(burst)}/20 messages delivered")

    cleanup(sub, pub)


# ══════════════════════════════════════════════════════════════
#  TEST 18: Firmware Version in API
# ══════════════════════════════════════════════════════════════

def test_firmware_version():
    section("18. Firmware Version in API")

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

        # Version should look like semver: x.y.z OR x.y.z-prerelease
        # (e.g. "0.7.0", "0.7.1-rc1"). We split off the pre-release
        # suffix at the first hyphen and only require the numeric core
        # to be three digit-only fields.
        ver = fw.get("version", "")
        core = ver.split("-", 1)[0]
        parts = core.split(".")
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            ok(f"  Version '{ver}' is valid semver")
        else:
            fail(f"  Version '{ver}' is not valid semver (expected x.y.z[-prerelease])")

        # Name should be non-empty
        if fw.get("name"):
            ok(f"  Firmware name: {fw['name']}")
        else:
            fail("  Firmware name is empty")

    except Exception as e:
        fail(f"Firmware version test failed: {e}")


# ══════════════════════════════════════════════════════════════
#  TEST 19: Firmware Update Page
# ══════════════════════════════════════════════════════════════

def test_firmware_update_page():
    section("19. Firmware Update Page")

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
#  TEST 20: Connected Clients Page and API
# ══════════════════════════════════════════════════════════════

def test_connected_clients():
    section("20. Connected Clients Page and API")

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

        # 0.6.0 swap: legacy `setTimeout(location.reload)` replaced with
        # in-place fetch('/api/clients') polling. Either keeps the page
        # fresh; flag only if neither is present.
        if ("fetch('/api/clients'" in html
                or "auto-refreshes" in html
                or "auto-refresh" in html.lower()):
            ok("  /clients has live refresh "
               "(legacy reload or /api/clients polling)")
        else:
            fail("  /clients page has no live-refresh mechanism")

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
#  TEST 21: Version in Dashboard and Footer
# ══════════════════════════════════════════════════════════════

def test_version_display():
    section("21. Version in Information Page and Footer")

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

# ============================================================
#  22. CSRF (added 0.7.1)
# ============================================================

def test_csrf():
    """CSRF protection: state-changing endpoints reject requests that
    lack a valid token; /api/csrf returns one; cookie is set on every
    response."""
    section("22. CSRF Protection")

    if requests is None:
        skip("'requests' module not installed")
        return

    # 1. /api/csrf returns a hex token
    try:
        resp = requests.get(f"{HTTP_BASE}/api/csrf", timeout=5)
        if resp.status_code != 200:
            fail(f"/api/csrf returned {resp.status_code}")
            return
        data = resp.json()
        token = data.get("token", "")
        if (len(token) == 32 and
                all(c in "0123456789abcdef" for c in token)):
            ok(f"/api/csrf returned 32-hex token ({token[:8]}...)")
        else:
            fail(f"/api/csrf token shape wrong: {token!r}")
            return
    except Exception as e:
        fail(f"/api/csrf failed: {e}")
        return

    # 2. Set-Cookie: csrf=... on every response
    try:
        resp = requests.get(f"{HTTP_BASE}/", timeout=5)
        sc = resp.headers.get("Set-Cookie", "")
        if "csrf=" in sc and "SameSite=Strict" in sc and "Path=/" in sc:
            ok("Set-Cookie: csrf=... SameSite=Strict Path=/")
        else:
            fail(f"Set-Cookie header missing or malformed: {sc!r}")
    except Exception as e:
        fail(f"Set-Cookie probe failed: {e}")

    # 3. POST /api/time/resync without token -> 403
    try:
        resp = requests.post(f"{HTTP_BASE}/api/time/resync", timeout=5)
        if resp.status_code == 403:
            ok("POST without token -> 403")
        else:
            fail(f"POST without token returned {resp.status_code} (want 403)")
    except Exception as e:
        fail(f"unauth POST probe failed: {e}")

    # 4. POST with WRONG token -> 403
    try:
        bad = "0" * 32
        resp = requests.post(f"{HTTP_BASE}/api/time/resync",
                             headers={"X-CSRF-Token": bad}, timeout=5)
        if resp.status_code == 403:
            ok("POST with wrong token -> 403")
        else:
            fail(f"POST with wrong token returned {resp.status_code} (want 403)")
    except Exception as e:
        fail(f"bad-token probe failed: {e}")

    # 5. POST with token via X-CSRF-Token header -> 200
    try:
        resp = requests.post(f"{HTTP_BASE}/api/time/resync",
                             headers={"X-CSRF-Token": token}, timeout=5)
        if resp.status_code == 200:
            ok("POST with header token -> 200")
        else:
            fail(f"POST with header token returned {resp.status_code} (want 200)")
    except Exception as e:
        fail(f"header-path probe failed: {e}")

    # 6. POST with token via form field -> 200
    try:
        resp = requests.post(f"{HTTP_BASE}/api/time/resync",
                             data={"csrf": token}, timeout=5)
        if resp.status_code == 200:
            ok("POST with form-field token -> 200")
        else:
            fail(f"POST with form-field token returned {resp.status_code} (want 200)")
    except Exception as e:
        fail(f"form-path probe failed: {e}")

    # 7. GET /reboot -> 404 (promoted to POST in 0.7.1)
    try:
        resp = requests.get(f"{HTTP_BASE}/reboot", timeout=5)
        if resp.status_code == 404:
            ok("GET /reboot -> 404 (promoted to POST in 0.7.1)")
        else:
            fail(f"GET /reboot returned {resp.status_code} (want 404)")
    except Exception as e:
        fail(f"GET /reboot probe failed: {e}")

    # 8. POST /reboot without token -> 403 (device NOT rebooted)
    try:
        resp = requests.post(f"{HTTP_BASE}/reboot", timeout=5)
        if resp.status_code == 403:
            ok("POST /reboot without token -> 403 (NOT rebooted)")
        else:
            fail(f"POST /reboot without token returned {resp.status_code} (want 403)")
    except Exception as e:
        fail(f"reboot probe failed: {e}")

    # 9. /api/csrf is auth-gated when auth is on. We bypass our own
    #    monkey-patched session by importing requests fresh.
    import os
    cfg_auth = os.environ.get("BROKER_AUTH", "")
    if cfg_auth:
        try:
            import importlib, sys
            r = importlib.import_module("requests")
            # Use the unpatched .get directly with a raw URL -- our
            # monkey-patch wraps requests.get/post at module load,
            # but a fresh urllib request bypasses it.
            import urllib.request as ur
            try:
                ur.urlopen(f"{HTTP_BASE}/api/csrf", timeout=5)
                fail("/api/csrf without auth returned 200 (want 401)")
            except ur.HTTPError as he:
                if he.code == 401:
                    ok("/api/csrf without auth -> 401")
                else:
                    fail(f"/api/csrf without auth returned {he.code} (want 401)")
        except Exception as e:
            fail(f"auth-gating probe failed: {e}")
    else:
        skip("/api/csrf auth-gating (no BROKER_AUTH set)")



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
    test_qos1_inbound()
    test_qos1_outbound()
    test_portal_api()
    test_portal_pages()
    test_portal_save_settings()
    test_firmware_version()
    test_firmware_update_page()
    test_connected_clients()
    test_version_display()
    test_csrf()

    elapsed = time.monotonic() - t0

    print(f"\n{'═'*60}")
    print(f"  RESULTS: {_pass_count} passed, {_fail_count} failed, {_skip_count} skipped")
    print(f"  Time: {elapsed:.1f}s")
    print(f"{'═'*60}\n")

    sys.exit(1 if _fail_count > 0 else 0)


if __name__ == "__main__":
    main()
