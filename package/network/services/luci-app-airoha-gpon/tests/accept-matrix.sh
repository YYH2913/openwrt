#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
program="$openwrt/package/network/services/luci-app-airoha-gpon/root/usr/sbin/airoha-xpon-accept-matrix"
temporary="$(mktemp -d)"
lock_pid=
trap 'status=$?; [ -z "$lock_pid" ] || kill "$lock_pid" 2>/dev/null || true; rm -rf "$temporary"; exit "$status"' EXIT INT TERM

mkdir -p "$temporary/bin"
state="$temporary/state"
edge_log="$temporary/edge.log"
hook_log="$temporary/hook.log"

cat > "$temporary/bin/collector" <<'EOF'
#!/bin/sh
printf 'runtime_mode=%s\n' "$(cat "$AIROHA_TEST_STATE")"
printf 'registration_identity=redacted\n'
EOF

cat > "$temporary/bin/edge" <<'EOF'
#!/bin/sh
set -eu
target="$1"
output="$2"
source="$(cat "$AIROHA_TEST_STATE")"
printf '%s>%s\n' "$source" "$target" >> "$AIROHA_TEST_EDGE_LOG"
mkdir -p "$output"
if [ "${AIROHA_TEST_FAIL_EDGE:-}" = "$source>$target" ]; then
	printf 'from_mode=%s\ntarget_mode=%s\nresult=target-not-ready-runtime-rolled-back\n' \
		"$source" "$target" > "$output/result.env"
	exit 1
fi
printf '%s\n' "$target" > "$AIROHA_TEST_STATE"
printf 'from_mode=%s\ntarget_mode=%s\nresult=%s\n' \
	"$source" "$target" "${AIROHA_TEST_EDGE_RESULT:-passed}" \
	> "$output/result.env"
EOF

cat > "$temporary/bin/hook" <<'EOF'
#!/bin/sh
printf '%s>%s\n' "$1" "$2" >> "$AIROHA_TEST_HOOK_LOG"
[ "${AIROHA_TEST_FAIL_HOOK:-}" != "$1>$2" ]
EOF
chmod +x "$temporary/bin/collector" "$temporary/bin/edge" \
	"$temporary/bin/hook"

run_matrix() {
	AIROHA_TEST_STATE="$state" \
	AIROHA_TEST_EDGE_LOG="$edge_log" \
	AIROHA_TEST_HOOK_LOG="$hook_log" \
	AIROHA_XPON_ACCEPT_COLLECTOR="$temporary/bin/collector" \
	AIROHA_XPON_ACCEPT_EDGE="$temporary/bin/edge" \
	AIROHA_XPON_ACCEPT_LOCK_FILE="$temporary/accept.lock" \
		sh "$program" "$@"
}

for start in xgpon xgspon epon-10g-1g epon-10g-10g; do
	output="$temporary/success-$start"
	printf '%s\n' "$start" > "$state"
	: > "$edge_log"
	run_matrix "$output" >/dev/null
	[ "$(cat "$state")" = "$start" ]
	[ "$(wc -l < "$edge_log")" -eq 12 ]
	[ "$(sort -u "$edge_log" | wc -l)" -eq 12 ]
	grep -Fx 'result=passed' "$output/result.env" >/dev/null
	grep -Fx 'completed_edges=12' "$output/result.env" >/dev/null
	grep -Fx 'total_edges=12' "$output/result.env" >/dev/null
	[ "$(find "$output" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 12 ]
	if grep -R -E 'HWTC12345678|subscriber' "$output" >/dev/null; then
		echo 'matrix evidence exposed registration identity' >&2
		exit 1
	fi
done

printf '%s\n' xgpon > "$state"
: > "$edge_log"
failed="$temporary/failed"
if AIROHA_TEST_FAIL_EDGE='xgpon>epon-10g-1g' run_matrix "$failed" \
		>/dev/null 2>&1; then
	echo 'failed directed edge did not stop the matrix' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
[ "$(wc -l < "$edge_log")" -eq 3 ]
grep -Fx 'result=edge-failed' "$failed/result.env" >/dev/null
grep -Fx 'completed_edges=2' "$failed/result.env" >/dev/null
grep -Fx 'failed_edge=3' "$failed/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$edge_log"
: > "$hook_log"
hook_failed="$temporary/hook-failed"
if AIROHA_XPON_ACCEPT_BEFORE_EDGE="$temporary/bin/hook" \
	AIROHA_TEST_FAIL_HOOK='xgpon>epon-10g-1g' \
		run_matrix "$hook_failed" >/dev/null 2>&1; then
	echo 'failed before-edge hook did not stop the matrix' >&2
	exit 1
fi
[ "$(cat "$state")" = xgpon ]
[ "$(wc -l < "$edge_log")" -eq 2 ]
[ "$(wc -l < "$hook_log")" -eq 3 ]
grep -Fx 'result=before-edge-hook-failed' "$hook_failed/result.env" >/dev/null

printf '%s\n' xgpon > "$state"
: > "$edge_log"
invalid="$temporary/invalid-evidence"
if AIROHA_TEST_EDGE_RESULT=unexpected run_matrix "$invalid" \
		>/dev/null 2>&1; then
	echo 'invalid per-edge evidence was accepted' >&2
	exit 1
fi
grep -Fx 'result=invalid-edge-evidence' "$invalid/result.env" >/dev/null
grep -Fx 'completed_edges=0' "$invalid/result.env" >/dev/null

lock_ready="$temporary/lock-ready"
(
	exec 7>"$temporary/accept.lock"
	flock -x 7
	: > "$lock_ready"
	sleep 1
) &
lock_pid=$!
while [ ! -e "$lock_ready" ]; do sleep 0.01; done
printf '%s\n' xgpon > "$state"
if run_matrix "$temporary/concurrent" >/dev/null 2>&1; then
	echo 'concurrent matrix was allowed to run' >&2
	exit 1
fi
wait "$lock_pid"
lock_pid=
[ ! -e "$temporary/concurrent" ]

echo 'Four-mode on-target 12-edge acceptance matrix: OK'
