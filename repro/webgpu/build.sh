#!/usr/bin/env bash
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(git -C "$HERE" rev-parse --show-toplevel)
OUT=${OUT:-$ROOT/.build/repro-webgpu}
BASE_REF=${BASE_REF:-HEAD~2}
PR_REF=${PR_REF:-HEAD}
CONTROL_REF=${CONTROL_REF:-}
JOBS=${JOBS:-$(nproc)}
CXX=${CXX:-g++}
BX=$(realpath "$ROOT/../bx")
BIMG=$(realpath "$ROOT/../bimg")

DAWN_TAG=v20260623.191042
DAWN_PKG=Dawn-636a470ddb7f53d1316ae740e2e843697bf8595a-ubuntu-latest-${DAWN_FLAVOR:-Release}.tar.gz
declare -A DAWN_SHA256=(
	[Release]=2272d6fca10066bfc6ead034a4f3f14d6fcd0d39d1c9dd3cac3fc3d330b9d139
	[Debug]=1ff962bbc07f60b0a050b52e3a49a21b38a9ceb422799a210ed5897942f6cc5f
)

for tool in git make curl sha256sum "$CXX" emcc em++ node; do
	command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done
export EMSCRIPTEN=${EMSCRIPTEN:-$(dirname "$(realpath "$(command -v emcc)")")}

WASM_CPPFLAGS="--use-port=emdawnwebgpu"
case "$(emcc -dM -E -x c /dev/null)" in
	*__EMSCRIPTEN_MAJOR__*) ;;
	# bx needs __EMSCRIPTEN_MAJOR__; newer emscripten only defines it lowercase in emscripten/version.h.
	*) WASM_CPPFLAGS="-include emscripten/version.h -D__EMSCRIPTEN_MAJOR__=__EMSCRIPTEN_major__ -D__EMSCRIPTEN_MINOR__=__EMSCRIPTEN_minor__ -D__EMSCRIPTEN_TINY__=__EMSCRIPTEN_tiny__ $WASM_CPPFLAGS" ;;
esac

setup_dawn() {
	local dir=$OUT/dawn
	[ -f "$dir/lib/libwebgpu_dawn.so" ] && return
	rm -rf "$dir"
	mkdir -p "$dir/lib"
	echo "dawn: $DAWN_TAG $DAWN_PKG"
	curl -fsSL --retry 3 -o "$dir/$DAWN_PKG" "https://github.com/google/dawn/releases/download/$DAWN_TAG/$DAWN_PKG"
	echo "${DAWN_SHA256[${DAWN_FLAVOR:-Release}]}  $dir/$DAWN_PKG" | sha256sum -c --quiet -
	tar -xzf "$dir/$DAWN_PKG" -C "$dir" --strip-components=1
	# bgfx dlopens this .so; the harness must link the same file so both share one Dawn.
	"$CXX" -shared -Wl,-soname,libwebgpu_dawn.so -o "$dir/lib/libwebgpu_dawn.so" \
		-Wl,--whole-archive "$dir/lib64/libwebgpu_dawn.a" -Wl,--no-whole-archive -lpthread -ldl -lm
}

checkout() {
	local name=$1 ref=$2 dir=$OUT/$1 sha
	sha=$(git -C "$ROOT" rev-parse --verify "$ref^{commit}")
	if [ -d "$dir/bgfx" ]; then
		git -C "$dir/bgfx" checkout -q --detach "$sha"
	else
		mkdir -p "$dir"
		git -C "$ROOT" worktree add -q --detach "$dir/bgfx" "$sha"
	fi
	ln -sfn "$BX" "$dir/bx"
	ln -sfn "$BIMG" "$dir/bimg"
	echo "$name: $(git -C "$dir/bgfx" log --oneline -1)"
}

build_native() {
	local name=$1 t=$OUT/$1/bgfx bin=$OUT/bin/$1
	local libs=("$t/.build/linux64_gcc/bin/libbgfxDebug.a" "$t/.build/linux64_gcc/bin/libbimgDebug.a" "$t/.build/linux64_gcc/bin/libbxDebug.a")
	local link=(-I"$t/include" -I"$BX/include" -I"$BIMG/include" -I"$OUT/dawn/include"
		"${libs[@]}" -L"$OUT/dawn/lib" -lwebgpu_dawn -Wl,-rpath,"$OUT/dawn/lib" -lX11 -lGL -ldl -lpthread)

	echo "$name: native bgfx (debug64)"
	(cd "$t" && ../bx/tools/bin/linux/genie --gcc=linux-gcc gmake >/dev/null)
	make -C "$t/.build/projects/gmake-linux-gcc" config=debug64 -j"$JOBS" bx bimg bgfx >"$OUT/$name/native-build.log" 2>&1 \
		|| { echo "failed, see $OUT/$name/native-build.log" >&2; exit 1; }

	mkdir -p "$bin"
	"$CXX" -std=c++20 -O1 -g -DBX_CONFIG_DEBUG=1 "$HERE/native/repro.cpp" "${link[@]}" -o "$bin/repro"

	echo "$name: native ThreadSanitizer variant"
	# Must link ahead of libbgfxDebug.a so the uninstrumented renderer_webgpu.o is never pulled in.
	"$CXX" -std=c++20 -g -O1 -m64 -mfpmath=sse -msse4.2 -fno-rtti -fno-exceptions -fsanitize=thread \
		-D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_CONSTANT_MACROS -D_DEBUG -DBX_CONFIG_DEBUG=1 \
		-I"$BX/include/compat/linux" -I"$t/3rdparty" -I"$t/3rdparty/khronos" -I"$BIMG/include" -I"$BX/include" -I"$t/include" \
		-c "$t/src/renderer_webgpu.cpp" -o "$bin/renderer_webgpu-tsan.o"
	"$CXX" -std=c++20 -O1 -g -fsanitize=thread -DBX_CONFIG_DEBUG=1 "$HERE/native/repro.cpp" "$bin/renderer_webgpu-tsan.o" "${link[@]}" -o "$bin/repro-tsan"
}

build_web() {
	local name=$1 t=$OUT/$1/bgfx web=$OUT/web/$1 obj=$OUT/web/obj/$1
	local libs=("$t/.build/wasm/bin/bgfxRelease.a" "$t/.build/wasm/bin/bimgRelease.a" "$t/.build/wasm/bin/bxRelease.a")
	local cxxflags=($WASM_CPPFLAGS -std=c++20 -O2 -fno-exceptions -fno-rtti -DNDEBUG -DBX_CONFIG_DEBUG=0
		-I"$t/include" -I"$BX/include" -iquote "$t/src")
	local link=(--use-port=emdawnwebgpu -O2 --profiling-funcs -sASSERTIONS=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
		-sEXPORTED_RUNTIME_METHODS=UTF8ToString)

	echo "$name: wasm bgfx (release, RENDERER_WEBGPU=1)"
	(cd "$t" && BGFX_CONFIG=RENDERER_WEBGPU=1 ../bx/tools/bin/linux/genie --gcc=wasm gmake >/dev/null)
	CPPFLAGS="$WASM_CPPFLAGS" make -C "$t/.build/projects/gmake-wasm" config=release -j"$JOBS" bx bimg bgfx >"$OUT/$name/wasm-build.log" 2>&1 \
		|| { echo "failed, see $OUT/$name/wasm-build.log" >&2; exit 1; }

	mkdir -p "$web" "$obj"
	em++ "${cxxflags[@]}" -DHARNESS_ASYNC=0 -c "$HERE/web/harness.cpp" -o "$obj/harness.o"
	em++ "${cxxflags[@]}" -DHARNESS_ASYNC=1 -c "$HERE/web/harness.cpp" -o "$obj/harness_async.o"
	em++ "$obj/harness.o" "${libs[@]}" "${link[@]}" -o "$web/harness.js"
	em++ "$obj/harness_async.o" "${libs[@]}" "${link[@]}" -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 -o "$web/harness_async.js"
}

mkdir -p "$OUT"
setup_dawn
names=(base)
checkout base "$BASE_REF"
if [ -n "$CONTROL_REF" ]; then
	checkout control "$CONTROL_REF"
	names+=(control)
else
	rm -rf "$OUT/bin/control" "$OUT/web/control"
fi
checkout pr "$PR_REF"
names+=(pr)
for name in "${names[@]}"; do
	build_native "$name"
	build_web "$name"
done
echo "done: run ./native.sh and node web.mjs"
