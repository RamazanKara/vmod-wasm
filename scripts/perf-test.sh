#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/perf-test.sh [options]

Run a short local performance benchmark inside the vmod-wasm Docker image.
The benchmark starts a backend and varnishd, then runs a Python socket load
generator against several VCL paths:

  baseline                 Varnish proxy only
  raw_execute              wasm.execute("test", "get_constant")
  proxy_request            proxy_wasm_on_request("pass")
  proxy_response           proxy_wasm_on_response("pass")
  proxy_response_body      proxy_wasm_on_response("pass") + wasm_body VDP
  proxy_response_rewrite   SDK response-body rewrite via set_buffer_bytes

Options:
  --duration SECONDS      Measured duration per case. Default: $DURATION or 10.
  --warmup SECONDS        Warmup duration per case. Default: $WARMUP or 2.
  --concurrency N         Parallel client connections. Default: $CONCURRENCY or 8.
  --pool-size N           Store pool size for Proxy-Wasm cases. Default: $POOL_SIZE or 64.
  --image NAME            Docker image. Default: $DOCKER_IMAGE or vmod-wasm-ci.
  --log-dir PATH          Host log directory. Default: perf-logs/<timestamp>.
  --build                 Build the Docker image before running.
  --help                  Show this help.

Environment equivalents are also supported:
  DURATION, WARMUP, CONCURRENCY, POOL_SIZE, DOCKER_IMAGE, LOG_DIR, BUILD_IMAGE.
EOF
}

DURATION="${DURATION:-10}"
WARMUP="${WARMUP:-2}"
CONCURRENCY="${CONCURRENCY:-8}"
POOL_SIZE="${POOL_SIZE:-64}"
DOCKER_IMAGE="${DOCKER_IMAGE:-vmod-wasm-ci}"
BUILD_IMAGE="${BUILD_IMAGE:-0}"
LOG_DIR="${LOG_DIR:-}"

while [ "$#" -gt 0 ]; do
	case "$1" in
	--duration)
		DURATION="$2"
		shift 2
		;;
	--warmup)
		WARMUP="$2"
		shift 2
		;;
	--concurrency)
		CONCURRENCY="$2"
		shift 2
		;;
	--pool-size)
		POOL_SIZE="$2"
		shift 2
		;;
	--image)
		DOCKER_IMAGE="$2"
		shift 2
		;;
	--log-dir)
		LOG_DIR="$2"
		shift 2
		;;
	--build)
		BUILD_IMAGE=1
		shift
		;;
	--help|-h)
		usage
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

case "$DURATION:$WARMUP:$CONCURRENCY:$POOL_SIZE" in
*[!0-9:]*)
	echo "Duration, warmup, concurrency, and pool size must be integers" >&2
	exit 2
	;;
esac

if [ "$DURATION" -le 0 ] || [ "$WARMUP" -lt 0 ] ||
    [ "$CONCURRENCY" -le 0 ] || [ "$POOL_SIZE" -le 0 ]; then
	echo "Duration/concurrency/pool size must be > 0; warmup must be >= 0" >&2
	exit 2
fi

if [ -z "$LOG_DIR" ]; then
	LOG_DIR="perf-logs/$(date -u +%Y%m%dT%H%M%SZ)"
fi

mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"

if [ "$BUILD_IMAGE" = 1 ] || ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
	docker build -t "$DOCKER_IMAGE" .
fi

echo "vmod-wasm performance test"
echo "  image:       $DOCKER_IMAGE"
echo "  duration:    ${DURATION}s"
echo "  warmup:      ${WARMUP}s"
echo "  concurrency: $CONCURRENCY"
echo "  pool size:   $POOL_SIZE"
echo "  logs:        $LOG_DIR"

docker run --rm -i \
	-e DURATION="$DURATION" \
	-e WARMUP="$WARMUP" \
	-e CONCURRENCY="$CONCURRENCY" \
	-e POOL_SIZE="$POOL_SIZE" \
	-v "$LOG_DIR:/perf-logs" \
	--entrypoint bash \
	"$DOCKER_IMAGE" \
	-s <<'CONTAINER'
set -Eeuo pipefail

duration="${DURATION}"
warmup="${WARMUP}"
concurrency="${CONCURRENCY}"
pool_size="${POOL_SIZE}"
secret=/tmp/vmod-wasm-perf.secret
admin="-T 127.0.0.1:6082 -S ${secret}"

log() {
	printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a /perf-logs/perf.log
}

cat > /tmp/perf_backend.py <<'PY'
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b"rewrite-me" if self.path.startswith("/rewrite") else b"OK\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


ThreadingHTTPServer(("127.0.0.1", 18080), Handler).serve_forever()
PY

cat > /tmp/loadgen.py <<'PY'
import argparse
import json
import socket
import statistics
import threading
import time


def read_response(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise OSError("connection closed while reading headers")
        data += chunk

    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    status = int(lines[0].split()[1])
    length = None
    for line in lines[1:]:
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
            break
    if length is None:
        raise OSError("missing content-length")

    body = rest
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise OSError("connection closed while reading body")
        body += chunk
    return status, body[:length]


def worker(args, stop_at, result):
    req = (
        f"GET {args.path} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "User-Agent: vmod-wasm-perf/1.0\r\n"
        "Connection: keep-alive\r\n\r\n"
    ).encode()
    expected = args.expect.encode() if args.expect is not None else None
    sock = None

    while time.monotonic() < stop_at:
        try:
            if sock is None:
                sock = socket.create_connection((args.host, args.port), timeout=2)
                sock.settimeout(2)
            start = time.perf_counter_ns()
            sock.sendall(req)
            status, body = read_response(sock)
            elapsed = time.perf_counter_ns() - start
            if status == 200 and (expected is None or body == expected):
                result["latencies"].append(elapsed)
                result["ok"] += 1
            else:
                result["errors"] += 1
        except Exception:
            result["errors"] += 1
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
                sock = None

    if sock is not None:
        try:
            sock.close()
        except Exception:
            pass


parser = argparse.ArgumentParser()
parser.add_argument("--label", required=True)
parser.add_argument("--host", default="127.0.0.1")
parser.add_argument("--port", type=int, default=6081)
parser.add_argument("--path", default="/")
parser.add_argument("--duration", type=float, required=True)
parser.add_argument("--concurrency", type=int, required=True)
parser.add_argument("--expect")
parser.add_argument("--quiet", action="store_true")
args = parser.parse_args()

stop_at = time.monotonic() + args.duration
results = [{"ok": 0, "errors": 0, "latencies": []} for _ in range(args.concurrency)]
threads = [threading.Thread(target=worker, args=(args, stop_at, r)) for r in results]

started = time.perf_counter()
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
elapsed = time.perf_counter() - started

latencies = []
ok = 0
errors = 0
for result in results:
    ok += result["ok"]
    errors += result["errors"]
    latencies.extend(result["latencies"])

latencies.sort()

def pct(p):
    if not latencies:
        return 0.0
    idx = int((len(latencies) - 1) * p / 100.0)
    return latencies[idx] / 1_000_000.0

summary = {
    "case": args.label,
    "duration_s": round(elapsed, 3),
    "concurrency": args.concurrency,
    "ok": ok,
    "errors": errors,
    "rps": round(ok / elapsed, 1) if elapsed > 0 else 0.0,
    "avg_ms": round((statistics.fmean(latencies) / 1_000_000.0), 3) if latencies else 0.0,
    "p50_ms": round(pct(50), 3),
    "p95_ms": round(pct(95), 3),
    "p99_ms": round(pct(99), 3),
}
if not args.quiet:
    print(json.dumps(summary, sort_keys=True), flush=True)
PY

write_vcl() {
	case_name="$1"
	cat > "/tmp/${case_name}.vcl" <<EOF
vcl 4.1;

import wasm from "/src/src/.libs/libvmod_wasm.so";

backend default {
    .host = "127.0.0.1";
    .port = "18080";
}

sub vcl_init {
    wasm.set_epoch_deadline(100);
EOF

	case "$case_name" in
	baseline)
		;;
	raw_execute)
		cat >> "/tmp/${case_name}.vcl" <<EOF
    wasm.load("test", "/src/tests/wasm/test_module.wasm");
EOF
		;;
	proxy_request|proxy_response|proxy_response_body)
		cat >> "/tmp/${case_name}.vcl" <<EOF
    wasm.load("pass", "/src/tests/wasm/passthrough.wasm");
    wasm.set_store_pool_size("pass", ${pool_size});
EOF
		;;
	proxy_response_rewrite)
		cat >> "/tmp/${case_name}.vcl" <<EOF
    wasm.load("sdk", "/src/tests/wasm/proxy_wasm_filter.wasm");
    wasm.set_store_pool_size("sdk", ${pool_size});
EOF
		;;
	esac

	cat >> "/tmp/${case_name}.vcl" <<'EOF'
}
EOF

	case "$case_name" in
	raw_execute)
		cat >> "/tmp/${case_name}.vcl" <<'EOF'

sub vcl_recv {
    set req.http.X-Wasm-Result = wasm.execute("test", "get_constant");
    if (req.http.X-Wasm-Result != "42") {
        return (synth(599, "wasm failed"));
    }
}
EOF
		;;
	proxy_request)
		cat >> "/tmp/${case_name}.vcl" <<'EOF'

sub vcl_recv {
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request("pass");
    if (req.http.X-Wasm-Action != "0") {
        return (synth(599, "wasm failed"));
    }
}
EOF
		;;
	proxy_response)
		cat >> "/tmp/${case_name}.vcl" <<'EOF'

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("pass");
}
EOF
		;;
	proxy_response_body)
		cat >> "/tmp/${case_name}.vcl" <<'EOF'

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("pass");
    set resp.filters += "wasm_body";
}
EOF
		;;
	proxy_response_rewrite)
		cat >> "/tmp/${case_name}.vcl" <<'EOF'

sub vcl_deliver {
    set resp.http.X-Wasm-Action = wasm.proxy_wasm_on_response("sdk");
    set resp.filters += "wasm_body";
}
EOF
		;;
	esac
}

start_varnish() {
	case_name="$1"
	rm -rf "/tmp/varnish-${case_name}"
	printf 'vmod-wasm-perf-secret\n' > "$secret"
	chmod 600 "$secret"
	varnishd \
		-F \
		-n "/tmp/varnish-${case_name}" \
		-a 127.0.0.1:6081 \
		-T 127.0.0.1:6082 \
		-S "$secret" \
		-f "/tmp/${case_name}.vcl" \
		-s malloc,256m \
		-p thread_pool_min=50 \
		-p thread_pool_max=1000 \
		-p thread_pool_stack=262144 \
		> "/perf-logs/varnishd-${case_name}.log" 2>&1 &
	varnish_pid=$!

	for _ in $(seq 1 100); do
		if varnishadm ${admin} status >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.1
	done
	log "FAIL: varnishd did not become ready for ${case_name}"
	return 1
}

stop_varnish() {
	varnishadm ${admin} stop >/dev/null 2>&1 || true
	if [ -n "${varnish_pid:-}" ]; then
		for _ in $(seq 1 50); do
			if ! kill -0 "$varnish_pid" >/dev/null 2>&1; then
				break
			fi
			sleep 0.1
		done
		if kill -0 "$varnish_pid" >/dev/null 2>&1; then
			kill "$varnish_pid" >/dev/null 2>&1 || true
		fi
		wait "$varnish_pid" >/dev/null 2>&1 || true
		varnish_pid=""
	fi
}

run_case() {
	case_name="$1"
	path="$2"
	expect="$3"

	write_vcl "$case_name"
	start_varnish "$case_name"

	if [ "$warmup" -gt 0 ]; then
		python3 /tmp/loadgen.py \
			--label "${case_name}-warmup" \
			--duration "$warmup" \
			--concurrency "$concurrency" \
			--path "$path" \
			--expect "$expect" \
			--quiet || true
	fi

	result="$(python3 /tmp/loadgen.py \
		--label "$case_name" \
		--duration "$duration" \
		--concurrency "$concurrency" \
		--path "$path" \
		--expect "$expect")"
	printf '%s\n' "$result" | tee -a /perf-logs/results.ndjson
	stop_varnish
}

cleanup() {
	set +e
	stop_varnish
	if [ -n "${backend_pid:-}" ]; then
		kill "$backend_pid" >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT INT TERM

python3 /tmp/perf_backend.py > /perf-logs/backend.log 2>&1 &
backend_pid=$!
for _ in $(seq 1 50); do
	if python3 - <<'PY' >/dev/null 2>&1
import socket
s = socket.create_connection(("127.0.0.1", 18080), timeout=1)
s.close()
PY
	then
		break
	fi
	sleep 0.1
done

: > /perf-logs/results.ndjson
run_case baseline / "OK"$'\n'
run_case raw_execute / "OK"$'\n'
run_case proxy_request / "OK"$'\n'
run_case proxy_response / "OK"$'\n'
run_case proxy_response_body / "OK"$'\n'
run_case proxy_response_rewrite /rewrite "rewrote-it"

python3 - <<'PY'
import json
from pathlib import Path

rows = [json.loads(line) for line in Path("/perf-logs/results.ndjson").read_text().splitlines() if line.strip()]
print("\ncase                       rps      avg_ms  p95_ms  p99_ms  errors")
print("-------------------------  -------  ------  ------  ------  ------")
for row in rows:
    print(f"{row['case']:<25}  {row['rps']:>7.1f}  {row['avg_ms']:>6.3f}  {row['p95_ms']:>6.3f}  {row['p99_ms']:>6.3f}  {row['errors']:>6}")
PY
CONTAINER
