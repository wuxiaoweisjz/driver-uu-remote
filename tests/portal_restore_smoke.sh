#!/bin/sh
set -eu

helper=${UU_CAPTURE_HELPER:-./build/uu-wayland-capture-helper}
wayland_run=${UU_WAYLAND_RUN:-$HOME/.local/libexec/uu-wayland-run}
port=${1:-47893}
restart_count=${2:-3}
token_file=${UU_PORTAL_RESTORE_TOKEN_FILE:-}
log_file=${UU_PORTAL_RESTORE_LOG:-/tmp/uu-portal-restore-smoke.log}
helper_pid=
temporary_token_dir=

if [ -z "$token_file" ]; then
	temporary_token_dir=$(mktemp -d)
	token_file=$temporary_token_dir/portal-restore-token
fi

cleanup()
{
	if [ -n "$helper_pid" ]; then
		kill "$helper_pid" 2>/dev/null || true
		wait "$helper_pid" 2>/dev/null || true
	fi
	if [ -n "$temporary_token_dir" ]; then
		rm -f -- "$token_file"
		rmdir -- "$temporary_token_dir" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

start_helper()
{
	UU_PORTAL_RESTORE_TOKEN_FILE="$token_file" "$wayland_run" \
		"$helper" "$port" >>"$log_file" 2>&1 &
	helper_pid=$!
}

stop_helper()
{
	kill "$helper_pid"
	wait "$helper_pid" 2>/dev/null || true
	helper_pid=
}

: >"$log_file"
start_helper
"$helper" --wait-ready "$port" 180
test -s "$token_file"
test "$(stat -c %a "$token_file")" = 600

iteration=1
while [ "$iteration" -le "$restart_count" ]; do
	stop_helper
	start_helper
	"$helper" --wait-ready "$port" 12
	test -s "$token_file"
	test "$(stat -c %a "$token_file")" = 600
	iteration=$((iteration + 1))
done

printf 'Portal restore smoke passed: %s automatic restarts on port %s\n' \
	"$restart_count" "$port"
