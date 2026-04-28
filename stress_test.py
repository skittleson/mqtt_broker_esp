#!/usr/bin/env python3
"""
MQTT Broker Stress Test for ESP32-S3
Tests: concurrent connections, message throughput, wildcard routing, latency
"""

import socket
import struct
import time
import threading
import sys
import statistics
from collections import defaultdict

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883

# ── MQTT helpers ──────────────────────────────────────────────

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

def mqtt_connect_pkt(client_id, keepalive=60):
    cid = client_id.encode()
    var_header = b'\x00\x04MQTT\x04\x02' + struct.pack('!H', keepalive)
    payload = struct.pack('!H', len(cid)) + cid
    remaining = var_header + payload
    return bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining

def mqtt_subscribe_pkt(packet_id, topic, qos=0):
    t = topic.encode()
    var_header = struct.pack('!H', packet_id)
    payload = struct.pack('!H', len(t)) + t + bytes([qos])
    remaining = var_header + payload
    return bytes([0x82]) + encode_remaining_length(len(remaining)) + remaining

def mqtt_publish_pkt(topic, message):
    t = topic.encode()
    m = message.encode() if isinstance(message, str) else message
    var_header = struct.pack('!H', len(t)) + t
    remaining = var_header + m
    return bytes([0x30]) + encode_remaining_length(len(remaining)) + remaining

def mqtt_pingreq():
    return bytes([0xC0, 0x00])

def mqtt_disconnect():
    return bytes([0xE0, 0x00])


def mqtt_connect_pkt_with_auth(client_id, username, password, keepalive=60):
    cid = client_id.encode()
    uname = username.encode()
    pwd = password.encode()
    # CONNECT flags: clean session(0x02), username(0x80), password(0x40)
    flags = 0x02 | 0x80 | 0x40
    var_header = b'\x00\x04MQTT\x04' + bytes([flags]) + struct.pack('!H', keepalive)
    payload = struct.pack('!H', len(cid)) + cid
    payload += struct.pack('!H', len(uname)) + uname
    payload += struct.pack('!H', len(pwd)) + pwd
    remaining = var_header + payload
    return bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining


def mqtt_connect_pkt_auth_only(client_id, username, keepalive=60):
    """CONNECT with username but no password (auth-only mode)."""
    cid = client_id.encode()
    uname = username.encode()
    flags = 0x02 | 0x80  # clean session + username, no password
    var_header = b'\x00\x04MQTT\x04' + bytes([flags]) + struct.pack('!H', keepalive)
    payload = struct.pack('!H', len(cid)) + cid
    payload += struct.pack('!H', len(uname)) + uname
    remaining = var_header + payload
    return bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining


def mqtt_connect_pkt_no_auth(client_id, keepalive=60):
    """CONNECT without username or password."""
    cid = client_id.encode()
    var_header = b'\x00\x04MQTT\x04\x02' + struct.pack('!H', keepalive)
    payload = struct.pack('!H', len(cid)) + cid
    remaining = var_header + payload
    return bytes([0x10]) + encode_remaining_length(len(remaining)) + remaining
class MQTTClient:
    def __init__(self, client_id):
        self.client_id = client_id
        self.sock = None
        self.connected = False

    def connect(self, timeout=5):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((HOST, PORT))
        self.sock.sendall(mqtt_connect_pkt(self.client_id))
        resp = self.sock.recv(4)
        if len(resp) >= 4 and resp[0] == 0x20 and resp[3] == 0x00:
            self.connected = True
            return True
        return False

    def connect_with_auth(self, username, password, timeout=5):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((HOST, PORT))
        self.sock.sendall(mqtt_connect_pkt_with_auth(self.client_id, username, password))
        resp = self.sock.recv(4)
        if len(resp) >= 4 and resp[0] == 0x20 and resp[3] == 0x00:
            self.connected = True
            return True, 0  # (success, return_code)
        return False, resp[3] if len(resp) > 3 else 0xFF

    def connect_with_auth_only(self, username, timeout=5):
        """Connect with username but no password (for auth-only config)."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((HOST, PORT))
        self.sock.sendall(mqtt_connect_pkt_auth_only(self.client_id, username))
        resp = self.sock.recv(4)
        if len(resp) >= 4 and resp[0] == 0x20 and resp[3] == 0x00:
            self.connected = True
            return True, 0
        return False, resp[3] if len(resp) > 3 else 0xFF

    def subscribe(self, topic, packet_id=1):
        self.sock.sendall(mqtt_subscribe_pkt(packet_id, topic))
        try:
            self.sock.recv(5)
        except socket.timeout:
            pass

    def publish(self, topic, message):
        self.sock.sendall(mqtt_publish_pkt(topic, message))

    def recv(self, timeout=1):
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(4096)
        except socket.timeout:
            return b''

    def ping(self):
        self.sock.sendall(mqtt_pingreq())
        try:
            resp = self.sock.recv(2)
            return len(resp) == 2 and resp[0] == 0xD0
        except socket.timeout:
            return False

    def disconnect(self):
        try:
            self.sock.sendall(mqtt_disconnect())
        except:
            pass
        try:
            self.sock.close()
        except:
            pass
        self.connected = False

# ── Test 1: Concurrent Connections ────────────────────────────

def test_concurrent_connections(target=90):
    """Try to connect `target` clients simultaneously."""
    print(f"\n{'='*60}")
    print(f"TEST 1: Concurrent Connections (target: {target})")
    print(f"{'='*60}")

    clients = []
    connect_times = []
    failures = 0

    for i in range(target):
        c = MQTTClient(f"stress-conn-{i:03d}")
        t0 = time.monotonic()
        try:
            ok = c.connect(timeout=10)
            dt = (time.monotonic() - t0) * 1000
            if ok:
                clients.append(c)
                connect_times.append(dt)
            else:
                failures += 1
                c.disconnect()
        except Exception as e:
            failures += 1
            dt = (time.monotonic() - t0) * 1000
            if i < 5 or i % 20 == 0:
                print(f"  Client {i} failed: {e}")

        # Progress
        if (i + 1) % 10 == 0:
            print(f"  Connected {len(clients)}/{i+1} (failures: {failures})")

    print(f"\n  Result: {len(clients)}/{target} connected, {failures} failures")
    if connect_times:
        print(f"  Connect time: min={min(connect_times):.0f}ms avg={statistics.mean(connect_times):.0f}ms max={max(connect_times):.0f}ms")

    # Verify all can still ping
    ping_ok = 0
    for c in clients:
        try:
            if c.ping():
                ping_ok += 1
        except:
            pass
    print(f"  Ping check: {ping_ok}/{len(clients)} responded")

    # Cleanup
    for c in clients:
        c.disconnect()
    time.sleep(1)

    return len(clients), failures

# ── Test 2: Message Throughput ────────────────────────────────

def test_message_throughput(num_messages=500, num_topics=50):
    """Publish many messages across topics, measure throughput."""
    print(f"\n{'='*60}")
    print(f"TEST 2: Message Throughput ({num_messages} msgs across {num_topics} topics)")
    print(f"{'='*60}")

    # Setup: 1 publisher, 5 subscribers each on 10 topics
    pub = MQTTClient("stress-pub")
    pub.connect()

    subs = []
    recv_counts = defaultdict(int)
    lock = threading.Lock()
    stop_event = threading.Event()

    for i in range(5):
        s = MQTTClient(f"stress-sub-{i}")
        s.connect()
        for t in range(i * 10, (i + 1) * 10):
            s.subscribe(f"bench/topic/{t:03d}", packet_id=t + 1)
        subs.append(s)
    time.sleep(0.5)

    # Receiver threads
    def receiver(sub, idx):
        sub.sock.settimeout(0.5)
        while not stop_event.is_set():
            try:
                data = sub.sock.recv(4096)
                if data:
                    # Count PUBLISH packets (type 0x30)
                    count = data.count(b'\x30')
                    with lock:
                        recv_counts[idx] += max(count, 1) if len(data) > 0 else 0
            except socket.timeout:
                continue
            except:
                break

    threads = []
    for i, s in enumerate(subs):
        t = threading.Thread(target=receiver, args=(s, i), daemon=True)
        t.start()
        threads.append(t)

    # Publish
    t0 = time.monotonic()
    for i in range(num_messages):
        topic = f"bench/topic/{i % num_topics:03d}"
        msg = f"msg-{i:06d}-{'x' * 50}"
        try:
            pub.publish(topic, msg)
        except Exception as e:
            print(f"  Publish failed at msg {i}: {e}")
            break
        # Small yield every 50 msgs to not overwhelm
        if i % 50 == 49:
            time.sleep(0.01)
    publish_time = time.monotonic() - t0

    # Wait for delivery
    time.sleep(3)
    stop_event.set()
    for t in threads:
        t.join(timeout=2)

    total_recv = sum(recv_counts.values())
    pub_rate = num_messages / publish_time

    print(f"\n  Published: {num_messages} messages in {publish_time:.2f}s ({pub_rate:.0f} msg/s)")
    print(f"  Received:  {total_recv} messages across {len(subs)} subscribers")
    for i, s in enumerate(subs):
        print(f"    Sub {i}: {recv_counts[i]} messages")

    # Cleanup
    pub.disconnect()
    for s in subs:
        s.disconnect()
    time.sleep(1)

    return pub_rate, total_recv

# ── Test 3: Wildcard Subscriptions ────────────────────────────

def test_wildcards():
    """Test + and # wildcard routing."""
    print(f"\n{'='*60}")
    print(f"TEST 3: Wildcard Subscriptions")
    print(f"{'='*60}")

    pub = MQTTClient("wild-pub")
    pub.connect()

    # Sub with + wildcard
    sub_plus = MQTTClient("wild-sub-plus")
    sub_plus.connect()
    sub_plus.subscribe("sensor/+/temperature", packet_id=1)

    # Sub with # wildcard
    sub_hash = MQTTClient("wild-sub-hash")
    sub_hash.connect()
    sub_hash.subscribe("sensor/#", packet_id=2)

    # Sub with exact match
    sub_exact = MQTTClient("wild-sub-exact")
    sub_exact.connect()
    sub_exact.subscribe("sensor/room1/temperature", packet_id=3)

    time.sleep(0.5)

    # Publish to various topics
    test_cases = [
        ("sensor/room1/temperature", "22.5"),   # matches all 3
        ("sensor/room2/temperature", "23.1"),   # matches + and #
        ("sensor/room1/humidity", "65"),         # matches # only
        ("sensor/room1/temperature/avg", "22"),  # matches # only
        ("other/topic", "nope"),                 # matches none
    ]

    for topic, msg in test_cases:
        pub.publish(topic, msg)
        time.sleep(0.1)

    time.sleep(1)

    # Collect results
    results = {}
    for name, sub in [("plus(+)", sub_plus), ("hash(#)", sub_hash), ("exact", sub_exact)]:
        data = sub.recv(timeout=1)
        # Count publish packets roughly
        count = 0
        idx = 0
        while idx < len(data):
            if data[idx] & 0xF0 == 0x30:
                count += 1
                # Skip this packet
                rl = data[idx + 1] if idx + 1 < len(data) else 0
                idx += 2 + rl
            else:
                idx += 1
        results[name] = count

    print(f"  sensor/+/temperature subscriber: {results.get('plus(+)', 0)} msgs (expected: 2)")
    print(f"  sensor/# subscriber:             {results.get('hash(#)', 0)} msgs (expected: 4)")
    print(f"  sensor/room1/temperature exact:   {results.get('exact', 0)} msgs (expected: 1)")

    passed = (results.get('plus(+)', 0) == 2 and
              results.get('hash(#)', 0) == 4 and
              results.get('exact', 0) == 1)
    print(f"  Result: {'PASS' if passed else 'FAIL'}")

    pub.disconnect()
    sub_plus.disconnect()
    sub_hash.disconnect()
    sub_exact.disconnect()
    time.sleep(1)

    return passed

# ── Test 4: Latency ───────────────────────────────────────────

def test_latency(num_samples=100):
    """Measure round-trip publish latency."""
    print(f"\n{'='*60}")
    print(f"TEST 4: Publish-to-Subscribe Latency ({num_samples} samples)")
    print(f"{'='*60}")

    pub = MQTTClient("lat-pub")
    pub.connect()
    sub = MQTTClient("lat-sub")
    sub.connect()
    sub.subscribe("latency/test", packet_id=1)
    time.sleep(0.5)

    latencies = []
    recv_thread_data = []
    lock = threading.Lock()

    def recv_loop():
        sub.sock.settimeout(0.1)
        while len(latencies) < num_samples:
            try:
                data = sub.sock.recv(4096)
                if data:
                    t_recv = time.monotonic()
                    with lock:
                        recv_thread_data.append((t_recv, data))
            except socket.timeout:
                continue
            except:
                break

    t = threading.Thread(target=recv_loop, daemon=True)
    t.start()

    send_times = {}
    for i in range(num_samples):
        msg = f"lat-{i:06d}"
        send_times[msg] = time.monotonic()
        pub.publish("latency/test", msg)
        time.sleep(0.02)  # 50 msg/s

    time.sleep(2)

    # Parse received messages and calculate latencies
    for t_recv, data in recv_thread_data:
        # Try to extract message from publish packet
        try:
            idx = 0
            while idx < len(data):
                if data[idx] & 0xF0 != 0x30:
                    idx += 1
                    continue
                rl = data[idx + 1]
                pkt_start = idx + 2
                topic_len = struct.unpack('!H', data[pkt_start:pkt_start + 2])[0]
                msg_start = pkt_start + 2 + topic_len
                msg_end = idx + 2 + rl
                msg = data[msg_start:msg_end].decode()
                if msg in send_times:
                    latencies.append((t_recv - send_times[msg]) * 1000)
                idx = msg_end
        except:
            pass

    t.join(timeout=2)

    if latencies:
        print(f"  Samples:  {len(latencies)}/{num_samples}")
        print(f"  Min:      {min(latencies):.1f} ms")
        print(f"  Avg:      {statistics.mean(latencies):.1f} ms")
        print(f"  Median:   {statistics.median(latencies):.1f} ms")
        print(f"  P95:      {sorted(latencies)[int(len(latencies)*0.95)]:.1f} ms")
        print(f"  Max:      {max(latencies):.1f} ms")
    else:
        print("  No latency samples collected!")

    pub.disconnect()
    sub.disconnect()
    time.sleep(1)

    return latencies


# ── Test 5: Authentication ────────────────────────────────────

def test_auth(username="esp32", password="esp32mqtt"):
    """Test MQTT authentication: correct creds, wrong creds, no creds, reconnect."""
    print(f"\n{'='*60}")
    print(f"TEST 5: Authentication")
    print(f"{'='*60}")

    all_pass = True

    # 5a: Connect with correct credentials
    print(f"\n  5a: Connect with correct credentials")
    c = MQTTClient("auth-correct")
    ok, rc = c.connect_with_auth(username, password)
    if ok:
        print(f"    PASS — connected with correct credentials")
        c.disconnect()
    else:
        print(f"    FAIL — expected CONNACK accepted, got rc={rc}")
        all_pass = False
    time.sleep(0.5)

    # 5b: Connect with wrong password
    print(f"  5b: Connect with wrong password")
    c = MQTTClient("auth-wrong-pwd")
    ok, rc = c.connect_with_auth(username, "wrong_password")
    if not ok and rc == 0x04:
        print(f"    PASS — rejected with CONNACK 0x04 (bad credentials)")
    else:
        print(f"    FAIL — expected CONNACK 0x04, got connected={ok} rc={rc}")
        all_pass = False
    c.disconnect()
    time.sleep(0.5)

    # 5c: Connect with wrong username
    print(f"  5c: Connect with wrong username")
    c = MQTTClient("auth-wrong-user")
    ok, rc = c.connect_with_auth("wrong_user", password)
    if not ok and rc == 0x04:
        print(f"    PASS — rejected with CONNACK 0x04 (bad credentials)")
    else:
        print(f"    FAIL — expected CONNACK 0x04, got connected={ok} rc={rc}")
        all_pass = False
    c.disconnect()
    time.sleep(0.5)

    # 5d: Connect without credentials (should be rejected when auth enabled)
    print(f"  5d: Connect without credentials (auth enabled)")
    c = MQTTClient("auth-no-creds")
    ok, rc = c.connect_with_auth_only("")
    if not ok and rc == 0x04:
        print(f"    PASS — rejected with CONNACK 0x04 (bad credentials)")
    else:
        print(f"    FAIL — expected CONNACK 0x04, got connected={ok} rc={rc}")
        all_pass = False
    c.disconnect()
    time.sleep(0.5)

    # 5e: Connect with username but empty password
    print(f"  5e: Connect with username, empty password")
    c = MQTTClient("auth-empty-pwd")
    ok, rc = c.connect_with_auth(username, "")
    if not ok and rc == 0x04:
        print(f"    PASS — rejected with CONNACK 0x04 (bad credentials)")
    else:
        print(f"    FAIL — expected CONNACK 0x04, got connected={ok} rc={rc}")
        all_pass = False
    c.disconnect()
    time.sleep(0.5)

    # 5f: Connect with auth, publish, and subscribe
    print(f"  5f: Authenticated client publishes and subscribes")
    sub = MQTTClient("auth-sub")
    ok, rc = sub.connect_with_auth(username, password)
    if not ok:
        print(f"    FAIL — auth subscriber could not connect (rc={rc})")
        all_pass = False
        sub.disconnect()
    else:
        sub.subscribe("auth/test", packet_id=1)
        time.sleep(0.3)

        pub = MQTTClient("auth-pub")
        ok, rc = pub.connect_with_auth(username, password)
        if not ok:
            print(f"    FAIL — auth publisher could not connect (rc={rc})")
            all_pass = False
            sub.disconnect()
            pub.disconnect()
        else:
            pub.publish("auth/test", "hello-auth")
            time.sleep(0.5)
            data = sub.sock.recv(4096)
            if b'hello-auth' in data:
                print(f"    PASS — authenticated subscriber received published message")
            else:
                print(f"    FAIL — subscriber did not receive published message")
                all_pass = False
            pub.disconnect()
        sub.disconnect()
        time.sleep(0.5)

    # 5g: Unauthenticated client attempts to subscribe (should be rejected)
    print(f"  5g: Unauthenticated client attempts SUBSCRIBE")
    c = MQTTClient("auth-unauth-sub")
    ok, rc = c.connect_with_auth_only("")
    if ok:
        # Should not happen if auth is enabled, but test anyway
        try:
            c.sock.sendall(mqtt_subscribe_pkt(1, "test"))
            resp = c.sock.recv(4)
            print(f"    INFO — unauthenticated client was not disconnected (may be expected if auth disabled)")
        except:
            print(f"    PASS — unauthenticated client disconnected before SUBSCRIBE")
        c.disconnect()
    else:
        print(f"    PASS — rejected before SUBSCRIBE (CONNACK 0x04)")
        c.disconnect()
        time.sleep(0.5)

    # 5h: Reconnect with same client ID after disconnect
    print(f"  5h: Reconnect with same client ID after disconnect")
    c1 = MQTTClient("auth-reconnect")
    ok1, rc1 = c1.connect_with_auth(username, password)
    if not ok1:
        print(f"    FAIL — first connection failed (rc={rc1})")
        all_pass = False
    else:
        c1.disconnect()
        time.sleep(0.5)

        c2 = MQTTClient("auth-reconnect")
        ok2, rc2 = c2.connect_with_auth(username, password)
        if ok2:
            print(f"    PASS — same client ID reconnected successfully")
            c2.disconnect()
        else:
            print(f"    FAIL — reconnect failed (rc={rc2})")
            all_pass = False
        time.sleep(0.5)

    print(f"\n  Result: {'PASS' if all_pass else 'FAIL'}")
    return all_pass

# ── Test 6: Many Topics ──────────────────────────────────────

def test_many_topics(num_topics=255):
    """Subscribe and publish to 255+ unique topics."""
    print(f"\n{'='*60}")
    print(f"TEST 5: Many Topics ({num_topics} unique topics)")
    print(f"{'='*60}")

    pub = MQTTClient("topics-pub")
    pub.connect()

    # Use multiple subscriber clients (each handles a batch of topics)
    batch_size = 50
    subs = []
    total_subscribed = 0

    for batch_start in range(0, num_topics, batch_size):
        batch_end = min(batch_start + batch_size, num_topics)
        s = MQTTClient(f"topics-sub-{batch_start}")
        try:
            s.connect()
            for t in range(batch_start, batch_end):
                s.subscribe(f"topic/{t:03d}", packet_id=t + 1)
                total_subscribed += 1
            subs.append(s)
        except Exception as e:
            print(f"  Sub client at {batch_start} failed: {e}")
            break
        time.sleep(0.1)

    print(f"  Subscribed to {total_subscribed} topics across {len(subs)} clients")
    time.sleep(0.5)

    # Publish one message to each topic
    pub_ok = 0
    t0 = time.monotonic()
    for t in range(num_topics):
        try:
            pub.publish(f"topic/{t:03d}", f"msg-{t}")
            pub_ok += 1
        except:
            break
        if t % 50 == 49:
            time.sleep(0.01)
    pub_time = time.monotonic() - t0

    time.sleep(2)

    # Count received
    total_recv = 0
    for s in subs:
        data = s.recv(timeout=1)
        if data:
            # Rough count of publish packets
            count = 0
            idx = 0
            while idx < len(data):
                if idx < len(data) and data[idx] & 0xF0 == 0x30:
                    count += 1
                    rl = data[idx + 1] if idx + 1 < len(data) else 0
                    idx += 2 + rl
                else:
                    idx += 1
            total_recv += count

    print(f"  Published: {pub_ok}/{num_topics} in {pub_time:.2f}s")
    print(f"  Received:  {total_recv} messages")

    pub.disconnect()
    for s in subs:
        s.disconnect()
    time.sleep(1)

    return pub_ok, total_recv

# ── Main ──────────────────────────────────────────────────────

if __name__ == "__main__":
    print(f"MQTT Broker Stress Test")
    print(f"Target: {HOST}:{PORT}")
    print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}")

    # Quick connectivity check
    try:
        c = MQTTClient("preflight")
        c.connect(timeout=5)
        c.disconnect()
    except Exception as e:
        print(f"FATAL: Cannot connect to broker: {e}")
        sys.exit(1)

    results = {}

    # Run tests
    connected, failures = test_concurrent_connections(target=90)
    results['connections'] = (connected, failures)

    pub_rate, total_recv = test_message_throughput(num_messages=500, num_topics=50)
    results['throughput'] = (pub_rate, total_recv)

    wildcard_pass = test_wildcards()
    results['wildcards'] = wildcard_pass

    latencies = test_latency(num_samples=100)
    results['latency'] = latencies

    pub_ok, recv_ok = test_many_topics(num_topics=255)
    results['topics'] = (pub_ok, recv_ok)

    auth_pass = test_auth()
    results['auth'] = auth_pass

    # Summary
    print(f"\n{'='*60}")
    print(f"SUMMARY")
    print(f"{'='*60}")
    c, f = results['connections']
    print(f"  Connections:  {c}/90 connected ({f} failures)")
    pr, tr = results['throughput']
    print(f"  Throughput:   {pr:.0f} msg/s publish, {tr} delivered")
    print(f"  Wildcards:    {'PASS' if results['wildcards'] else 'FAIL'}")
    if results['latency']:
        lat = results['latency']
        print(f"  Latency:      {statistics.mean(lat):.1f}ms avg, {max(lat):.1f}ms max")
    po, ro = results['topics']
    print(f"  Topics:       {po}/255 published, {ro} delivered")
    print(f"  Auth:         {'PASS' if results['auth'] else 'FAIL'}")
    print(f"{'='*60}")
