#!/bin/bash
# Build the guest-free gates against the whole runtime at -O2 and run them.
# Evidence for the 00-MASTER item 1 recommendation ("-O2 for the runtime"):
# the bench (make perf-O2) shows what -O2 buys; this shows what it breaks.
set -u
cd "$(dirname "$0")/.."
CF="-g -O2 -fobjc-arc -Wall -Wextra -Wno-unused-parameter -arch arm64"
INC="-Iruntime -Iruntime/jni -Iruntime/libc -Iruntime/gfx -Iruntime/xr -Iruntime/media -Iruntime/guest -Iruntime/diag"
LIBS="-lz -framework AudioToolbox -framework CoreAudio -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface -framework CoreFoundation -framework AVFoundation -framework Foundation"
SRCS=$(ls runtime/*.c runtime/jni/*.c runtime/libc/*.c runtime/gfx/*.c runtime/xr/*.c runtime/media/*.c runtime/guest/*.c runtime/diag/*.c | grep -v kl_view.c)
SRCS="$SRCS $(ls runtime/gfx/*.m runtime/media/*.m | grep -v kl_view_mtl.m)"
SRCS="$SRCS runtime/kl_stub_cells.S runtime/libc/kl_va_thunks.S runtime/xr/kl_ovrp_sret.S runtime/gfx/kl_gl_trace.S runtime/gfx/kl_gl_lock.S runtime/media/kl_phonon_hrtf.S runtime/media/kl_menu_font.S"
for t in t_variadic t_haptics t_softinput t_xrspace t_xrinput t_ctr t_fault t_bcast; do
  EXTRA=""; [ "$t" = t_variadic ] && EXTRA="tests/t_variadic_call.S"
  if ! clang $CF $INC -o "build/${t}_O2" "tests/$t.c" $EXTRA $SRCS $LIBS >"/tmp/o2_$t.log" 2>&1; then
    echo "$t@O2: BUILD FAIL"; grep -m2 "error" "/tmp/o2_$t.log"; continue
  fi
  out=$(./build/${t}_O2 2>&1); rc=$?
  echo "$t@O2: rc=$rc $(echo "$out" | tail -1 | cut -c1-80)"
done
