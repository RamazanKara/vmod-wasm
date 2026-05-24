#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/soak-test.sh [options]

Run a real varnishd soak test inside the vmod-wasm Docker image. The test
starts a backend, loads the VMOD from the built tree, sends concurrent traffic,
periodically reloads VCL, and writes logs/stats to the host.

Options:
  --duration SECONDS        Run length. Default: $DURATION or 3600.
  --concurrency N           Parallel client workers. Default: $CONCURRENCY or 8.
  --reload-interval SECONDS VCL reload cadence. Default: $RELOAD_INTERVAL or 60.
  --sample-interval SECONDS Stats sample cadence. Default: $SAMPLE_INTERVAL or 30.
  --image NAME              Docker image. Default: $DOCKER_IMAGE or vmod-wasm-ci.
  --log-dir PATH            Host log directory. Default: soak-logs/<timestamp>.
  --build                   Build the Docker image before running.
  --help                    Show this help.

Environment equivalents are also supported:
  DURATION, CONCURRENCY, RELOAD_INTERVAL, SAMPLE_INTERVAL, DOCKER_IMAGE,
  LOG_DIR, BUILD_IMAGE.
EOF
}

DURATION="${DURATION:-3600}"
CONCURRENCY="${CONCURRENCY:-8}"
RELOAD_INTERVAL="${RELOAD_INTERVAL:-60}"
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-30}"
DOCKER_IMAGE="${DOCKER_IMAGE:-vmod-wasm-ci}"
BUILD_IMAGE="${BUILD_IMAGE:-0}"
LOG_DIR="${LOG_DIR:-}"

while [ "$#" -gt 0 ]; do
	case "$1" in
	--duration)
		DURATION="$2"
		shift 2
		;;
	--concurrency)
		CONCURRENCY="$2"
		shift 2
		;;
	--reload-interval)
		RELOAD_INTERVAL="$2"
		shift 2
		;;
	--sample-interval)
		SAMPLE_INTERVAL="$2"
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

case "$DURATION:$CONCURRENCY:$RELOAD_INTERVAL:$SAMPLE_INTERVAL" in
*[!0-9:]*)
	echo "Duration, concurrency, reload interval, and sample interval must be integers" >&2
	exit 2
	;;
esac

if [ "$DURATION" -le 0 ] || [ "$CONCURRENCY" -le 0 ] ||
    [ "$RELOAD_INTERVAL" -le 0 ] || [ "$SAMPLE_INTERVAL" -le 0 ]; then
	echo "Duration, concurrency, reload interval, and sample interval must be > 0" >&2
	exit 2
fi

if [ -z "$LOG_DIR" ]; then
	LOG_DIR="soak-logs/$(date -u +%Y%m%dT%H%M%SZ)"
fi

mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"

if [ "$BUILD_IMAGE" = 1 ] || ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
	docker build -t "$DOCKER_IMAGE" .
fi

echo "vmod-wasm soak test"
echo "  image:           $DOCKER_IMAGE"
echo "  duration:        ${DURATION}s"
echo "  concurrency:     $CONCURRENCY"
echo "  reload interval: ${RELOAD_INTERVAL}s"
echo "  sample interval: ${SAMPLE_INTERVAL}s"
echo "  logs:            $LOG_DIR"

docker run --rm -i \
	-e DURATION="$DURATION" \
	-e CONCURRENCY="$CONCURRENCY" \
	-e RELOAD_INTERVAL="$RELOAD_INTERVAL" \
	-e SAMPLE_INTERVAL="$SAMPLE_INTERVAL" \
	-v "$LOG_DIR:/soak-logs" \
	--entrypoint bash \
	"$DOCKER_IMAGE" \
	-s <<'CONTAINER'
set -Eeuo pipefail

duration="${DURATION}"
concurrency="${CONCURRENCY}"
reload_interval="${RELOAD_INTERVAL}"
sample_interval="${SAMPLE_INTERVAL}"
end_ts=$(( $(date +%s) + duration ))
secret=/tmp/vmod-wasm-soak.secret
admin="-T 127.0.0.1:6082 -S ${secret}"
current_vcl="boot"
reloads=0
reload_errors=0

log() {
	printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a /soak-logs/soak.log
}

write_vcl() {
	name="$1"
	marker="$2"
	cat > "/tmp/${name}.vcl" <<EOF
vcl 4.1;

import wasm from "/src/src/.libs/libvmod_wasm.so";

backend default {
    .host = "127.0.0.1";
    .port = "18080";
}

sub vcl_init {
    wasm.set_memory_limit(67108864);
    wasm.set_epoch_deadline(100);
    wasm.load("test", "/src/tests/wasm/test_module.wasm");
    wasm.load("edge", "/src/tests/wasm/edge_security_filter.wasm");
}

sub vcl_recv {
    set req.http.X-Wasm-Result = wasm.execute("test", "get_constant");
    set req.http.X-Wasm-Action = wasm.proxy_wasm_on_request_configured("edge", "",
        {"{"rate_limit":{"requests_per_second":1000000,"window_seconds":60},"bot_patterns":[],"blocked_countries":[],"enrich_headers":true}"});

    if (req.http.X-Wasm-Result != "42" || req.http.X-Wasm-Action != "0") {
        return (synth(599, "wasm failed"));
    }
}

sub vcl_deliver {
    set resp.http.X-Wasm-Result = req.http.X-Wasm-Result;
    set resp.http.X-Wasm-Resp = wasm.proxy_wasm_on_response_configured("edge", "",
        {"{"rate_limit":{"requests_per_second":1000000,"window_seconds":60},"bot_patterns":[],"blocked_countries":[],"enrich_headers":true}"});
    set resp.http.X-Wasm-Version = wasm.version();
    set resp.http.X-Soak-VCL = "${marker}";
}
EOF
}

cat > /tmp/soak_backend.py <<'PY'
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = b"OK\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return

ThreadingHTTPServer(("127.0.0.1", 18080), Handler).serve_forever()
PY

cleanup() {
	set +e
	for pid in ${worker_pids:-} ${reloader_pid:-} ${sampler_pid:-} ${vlog_pid:-}; do
		if [ -n "${pid}" ]; then
			kill "$pid" >/dev/null 2>&1 || true
		fi
	done
	if [ -n "${backend_pid:-}" ]; then
		kill "$backend_pid" >/dev/null 2>&1 || true
	fi
	varnishadm ${admin} stop >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

python3 /tmp/soak_backend.py > /soak-logs/backend.log 2>&1 &
backend_pid=$!

for _ in $(seq 1 50); do
	if curl -fsS --max-time 1 http://127.0.0.1:18080/ >/dev/null 2>&1; then
		break
	fi
	sleep 0.1
done

printf 'vmod-wasm-soak-secret\n' > "$secret"
chmod 600 "$secret"
write_vcl "$current_vcl" "$current_vcl"

log "starting varnishd"
varnishd \
	-a 127.0.0.1:6081 \
	-T 127.0.0.1:6082 \
	-S "$secret" \
	-f "/tmp/${current_vcl}.vcl" \
	-s malloc,256m \
	-p thread_pool_min=50 \
	-p thread_pool_max=1000 \
	-p thread_pool_stack=262144 \
	> /soak-logs/varnishd.log 2>&1

for _ in $(seq 1 100); do
	if varnishadm ${admin} status >/dev/null 2>&1; then
		break
	fi
	sleep 0.1
done
if ! varnishadm ${admin} status >/dev/null 2>&1; then
	log "FAIL: varnishd did not become ready"
	exit 1
fi
varnishadm ${admin} status | tee -a /soak-logs/admin.log

varnishlog -g raw -i Error,FetchError > /soak-logs/varnish-errors.log 2>&1 &
vlog_pid=$!

sample_loop() {
	while [ "$(date +%s)" -lt "$end_ts" ]; do
		{
			printf '\n== %s ==\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
			varnishadm ${admin} status || true
			varnishadm ${admin} vcl.list || true
			varnishstat -1 | awk '
				$1 ~ /^(MAIN.uptime|MAIN.client_req|MAIN.cache_hit|MAIN.cache_miss|MAIN.sess_conn|MAIN.threads|MAIN.threads_limited|MAIN.threads_failed|MAIN.n_lru_nuked|MAIN.backend_fail|MGT.child_(panic|died)|VBE.*.happy)$/ { print }
			'
		} >> /soak-logs/stats.log 2>&1
		sleep "$sample_interval"
	done
}

reload_loop() {
	local idx=0 old next
	while [ "$(date +%s)" -lt "$end_ts" ]; do
		sleep "$reload_interval"
		[ "$(date +%s)" -lt "$end_ts" ] || break
		idx=$((idx + 1))
		old="$current_vcl"
		next="soak_${idx}"
		write_vcl "$next" "$next"
		{
			printf '\n== reload %s at %s ==\n' "$next" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
			if varnishadm ${admin} vcl.load "$next" "/tmp/${next}.vcl" &&
			    varnishadm ${admin} vcl.use "$next"; then
				reloads=$((reloads + 1))
				current_vcl="$next"
				if [ "$old" != "boot" ]; then
					varnishadm ${admin} vcl.discard "$old" || true
				fi
			else
				reload_errors=$((reload_errors + 1))
			fi
			varnishadm ${admin} vcl.list || true
			printf '%s %s\n' "$reloads" "$reload_errors" > /soak-logs/reload.stats
		} >> /soak-logs/reload.log 2>&1
	done
	printf '%s %s\n' "$reloads" "$reload_errors" > /soak-logs/reload.stats
}

worker_loop() {
	local id="$1" ok=0 err=0 code headers
	headers="/tmp/soak-worker-${id}.headers"
	while [ "$(date +%s)" -lt "$end_ts" ]; do
		: > "$headers"
		code="$(
			curl -sS --max-time 5 \
				-A "vmod-wasm-soak/1.0" \
				-D "$headers" \
				-o /dev/null \
				-w '%{http_code}' \
				"http://127.0.0.1:6081/soak/${id}/${ok}?t=$(date +%s%N)" \
				2>>"/soak-logs/curl-${id}.err" || true
		)"
		if [ "$code" = "200" ] &&
		    grep -qi '^X-Wasm-Result:[[:space:]]*42' "$headers" &&
		    grep -qi '^X-Wasm-Resp:[[:space:]]*0' "$headers" &&
		    grep -qi '^X-Wasm-Version:[[:space:]]*4\.3\.3' "$headers"; then
			ok=$((ok + 1))
		else
			err=$((err + 1))
			{
				printf 'worker=%s code=%s at=%s\n' "$id" "$code" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
				cat "$headers"
				printf '\n'
			} >> "/soak-logs/worker-${id}.bad"
			sleep 0.1
		fi
	done
	printf '%s %s\n' "$ok" "$err" > "/soak-logs/worker-${id}.stats"
}

sample_loop &
sampler_pid=$!
reload_loop &
reloader_pid=$!

worker_pids=""
for id in $(seq 1 "$concurrency"); do
	worker_loop "$id" &
	worker_pids="${worker_pids} $!"
done

log "traffic started: duration=${duration}s concurrency=${concurrency} reload_interval=${reload_interval}s"

for pid in $worker_pids; do
	wait "$pid"
done
wait "$reloader_pid" || true
wait "$sampler_pid" || true

total_ok=0
total_err=0
for f in /soak-logs/worker-*.stats; do
	[ -f "$f" ] || continue
	read -r ok err < "$f"
	total_ok=$((total_ok + ok))
	total_err=$((total_err + err))
done

if [ -f /soak-logs/reload.stats ]; then
	read -r reloads reload_errors < /soak-logs/reload.stats
else
	reloads=0
	reload_errors=0
fi

panic=""
panic="$(varnishadm ${admin} panic.show 2>&1 || true)"
printf '%s\n' "$panic" > /soak-logs/panic.show

{
	echo "duration=${duration}"
	echo "concurrency=${concurrency}"
	echo "reload_interval=${reload_interval}"
	echo "sample_interval=${sample_interval}"
	echo "requests_ok=${total_ok}"
	echo "requests_error=${total_err}"
	echo "reloads=${reloads}"
	echo "reload_errors=${reload_errors}"
} | tee /soak-logs/summary.env

log "finished: requests_ok=${total_ok} requests_error=${total_err} reloads=${reloads} reload_errors=${reload_errors}"

if [ "$total_ok" -eq 0 ]; then
	log "FAIL: no successful requests"
	exit 1
fi
if [ "$total_err" -ne 0 ]; then
	log "FAIL: client errors observed"
	exit 1
fi
if [ "$reload_errors" -ne 0 ]; then
	log "FAIL: VCL reload errors observed"
	exit 1
fi
if printf '%s\n' "$panic" | grep -Eiq 'Assert error|Panic at|Signal|Segmentation|Backtrace'; then
	log "FAIL: panic output is not clean"
	exit 1
fi

log "PASS"
CONTAINER
