#!/usr/bin/env bash
# Checks the portable layers in appimage/out/layer (made by build-layer.sh):
#   a) no glibc symbol version above 2.35, no GLIBCXX/CXXABI/GCC imports, NEEDED only libc, libm,
#      libpthread, libdl and the dynamic loader
#   b) the exported symbols: the native build's Vulkan entry points (build/fg/libsc_fg.so,
#      build/libsc_probe.so, read only; the native build's libstdc++ template instances are left
#      out of the comparison, the portable build keeps them inside on purpose)
#   c) each library loaded (dlopen RTLD_NOW, interface negotiated, unloaded) in clean containers:
#      ubuntu:22.04, debian:12, fedora:44 must pass; the native build must fail in ubuntu:22.04 (which
#      shows that the check can fail). (No older system as a control: the test program, built
#      against glibc 2.35, would itself not start there.)
# Prints PASS/FAIL per check; exit 0 when all pass. The functional test on the GPU is
# appimage/test-layer-headless.sh.
set -uo pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
project="$(dirname "$here")"
out="$here/out/layer"
work="$here/work"
podman="${PODMAN:-podman}"
name=myrddin-layer-build
fail=0
result() { printf '%-4s %s\n' "$1" "$2"; [ "$1" = FAIL ] && fail=1; return 0; }
libs="fg/libsc_fg.so libsc_probe.so"

for lib in $libs; do
    f="$out/$lib"
    [ -f "$f" ] || { result FAIL "$lib missing (appimage/build-layer.sh)"; continue; }
    max="$(objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    cxx="$(objdump -T "$f" | grep -c 'GLIBCXX\|CXXABI\|(GCC_')"
    needed="$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
    other="$(printf '%s\n' $needed | grep -vxE 'libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|ld-linux-x86-64\.so\.2')"
    if [ "$(printf '%s\n' "$max" GLIBC_2.35 | sort -V | tail -1)" = GLIBC_2.35 ] && [ "$cxx" = 0 ] && [ -z "$other" ]; then
        result PASS "a $lib: newest $max, $cxx C++ runtime imports, NEEDED $needed"
    else
        result FAIL "a $lib: newest $max, $cxx C++ runtime imports, NEEDED $needed"
    fi
    case "$lib" in fg/*) native="$project/build/fg/libsc_fg.so" ;; *) native="$project/build/libsc_probe.so" ;; esac
    mine="$(nm -D --defined-only "$f" | awk '{print $3}' | sort | tr '\n' ' ')"
    if [ -f "$native" ]; then
        theirs="$(nm -D --defined-only "$native" | awk '$3 !~ /^_Z/ {print $3}' | sort | tr '\n' ' ')"
        leaked="$(nm -D --defined-only "$native" | awk '$3 ~ /^_Z/' | wc -l)"
        if [ "$mine" = "$theirs" ] && [ -z "$(nm -D --defined-only "$f" | awk '$3 ~ /^_Z/')" ]; then
            result PASS "b $lib: exports $mine(native: the same + $leaked C++ template instances)"
        else
            result FAIL "b $lib: exports $mine; native $theirs"
        fi
    else
        [ "$mine" = "vkNegotiateLoaderLayerInterfaceVersion " ] && r=PASS || r=FAIL
        result $r "b $lib: exports $mine(no native build to compare with)"
    fi
done

# c) the loader's view, in clean containers
mkdir -p "$work/test"
"$podman" exec "$name" bash -c 'cc -O2 -Wall -isystem /work/deps/vulkan-headers-1.4.341/include -o /work/test/dlopen-test /src/appimage/tests/dlopen-test.c -ldl' \
    || { result FAIL "c: dlopen-test not built (is the container $name there? appimage/build-layer.sh)"; exit 1; }
run_in() {   # run_in IMAGE EXPECT(pass|fail) DIR LABEL
    local image=$1 expect=$2 dir=$3 label=$4 log rc
    if ! "$podman" image exists "$image" && ! nice -n 19 "$podman" pull -q "$image" > /dev/null 2>&1; then
        result SKIP "c $label in $image: image not available"; return
    fi
    log="$("$podman" run --rm --network none --security-opt label=disable -v "$dir:/layer:ro" -v "$work/test:/t:ro" \
        "$image" /t/dlopen-test /layer/fg/libsc_fg.so /layer/libsc_probe.so 2>&1)"; rc=$?
    glibc="$("$podman" run --rm --network none "$image" sh -c 'ldd --version 2>&1 | head -1' | grep -o '[0-9]\+\.[0-9]\+$')"
    if { [ $rc = 0 ] && [ $expect = pass ]; } || { [ $rc != 0 ] && [ $expect = fail ]; }; then
        result PASS "c $label in $image (glibc $glibc), expected to $expect:"
    else
        result FAIL "c $label in $image (glibc $glibc), expected to $expect:"
    fi
    echo "$log" | sed 's/^/       /'
}
native_dir="$work/test/native"; mkdir -p "$native_dir/fg"
cp -p "$project/build/fg/libsc_fg.so" "$native_dir/fg/" 2>/dev/null; cp -p "$project/build/libsc_probe.so" "$native_dir/" 2>/dev/null
run_in docker.io/library/ubuntu:22.04 pass "$out" portable
run_in docker.io/library/debian:12 pass "$out" portable
run_in registry.fedoraproject.org/fedora:44 pass "$out" portable
[ -f "$native_dir/fg/libsc_fg.so" ] && run_in docker.io/library/ubuntu:22.04 fail "$native_dir" native
exit $fail
