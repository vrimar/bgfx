#!/usr/bin/env bash
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(git -C "$HERE" rev-parse --show-toplevel)
OUT=${OUT:-$ROOT/.build/repro-webgpu}
LOG=$OUT/native-$(date +%Y%m%dT%H%M%S).log
unexpected=0

expected() {
	local item items
	IFS=, read -ra items <<<"$1"
	for item in "${items[@]}"; do
		[ "${item%%=*}" = "$2" ] && { echo "${item#*=}"; return 0; }
	done
	return 1
}

report() {
	local build=$1 test=$2 want=$3 verdict=$4 detail=$5 mark
	case "$verdict" in
		INFO) mark=info ;;
		OK|BUG) if [ "${verdict,,}" = "$want" ]; then mark="as expected"; else mark=UNEXPECTED; ((++unexpected)); fi ;;
		*) mark=UNEXPECTED; ((++unexpected)) ;;
	esac
	printf '%-7s %-52s %-4s %-11s  %s\n' "$build" "$test" "$verdict" "$mark" "$detail"
}

run() {
	local spec=$1 bin=$2 build want line verdict detail
	shift 2
	for build in base control pr; do
		want=$(expected "$spec" "$build") || continue
		[ -x "$OUT/bin/$build/$bin" ] || continue
		echo "=== $build $bin $*" >>"$LOG"
		verdict=NONE detail="no result, see $LOG"
		while IFS= read -r line; do
			[[ $line == "RESULT "* ]] || continue
			line=${line#RESULT }
			verdict=${line%% *}
			detail=${line#* }
		done < <(timeout 300 "$OUT/bin/$build/$bin" "$@" 2>>"$LOG")
		report "$build" "$bin $*" "$want" "$verdict" "$detail"
	done
}

tsan() {
	local spec=$1 build want log races verdict
	for build in base control pr; do
		want=$(expected "$spec" "$build") || continue
		[ -x "$OUT/bin/$build/repro-tsan" ] || continue
		for i in 1 2 3; do
			log=$OUT/tsan-$build-$i.log
			TSAN_OPTIONS="halt_on_error=0 history_size=4 ${TSAN_OPTIONS:-}" timeout 600 "$OUT/bin/$build/repro-tsan" pump --frames 300 --views 130 >"$log" 2>&1
			races=$(grep -c 'WARNING: ThreadSanitizer: data race' "$log")
			if [ "$races" -gt 0 ]; then verdict=BUG; elif grep -q '^RESULT OK completed' "$log"; then verdict=OK; else verdict=NONE; fi
			report "$build" "repro-tsan pump --frames 300 --views 130" "$want" "$verdict" "$races data races, log $log"
		done
	done
}

echo "-- DRAW_INDIRECT matches the app device's IndirectFirstInstance"
run control=ok,pr=ok repro caps
run control=bug,pr=ok repro caps --no-indirect

echo "-- the app's AllowProcessEvents callback runs only when the app pumps, on its thread"
for i in 1 2 3; do run control=bug,pr=ok repro appcb --frames 30; done

echo "-- app forces device loss: bgfx reports DeviceLost and falls back to Noop"
run control=bug,pr=ok repro lost --frames 60 --views 16

echo "-- readback returns the rendered pixel"
run control=ok,pr=ok repro readback
run base=ok,control=ok,pr=ok repro readback --own

echo "-- waiting on GPU-bound frames, bgfx's own device, uses under half a core"
run base=bug,control=bug,pr=ok repro cpu --own --size 8192 --views 24 --frames 150

echo "-- app pumps wgpuInstanceProcessEvents on another thread while bgfx renders"
run control=ok,pr=ok repro pump --frames 3000 --views 130

if [ "${1:-}" = --tsan ]; then
	echo "-- same under ThreadSanitizer (needs a Mesa Vulkan driver, see README)"
	tsan control=bug,pr=ok
fi

echo "log: $LOG"
if [ "$unexpected" -eq 0 ]; then
	echo "all results as expected"
else
	echo "$unexpected UNEXPECTED result(s)"
	exit 1
fi
