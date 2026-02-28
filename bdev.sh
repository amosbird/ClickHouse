#!/usr/bin/env bash

set -e
# set -x

cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

type=$1
if [ -z "$type" ]; then
    bdir=$(basename "$(readlink -f build)")
    type="${bdir#build-}"
else
    bdir=build-$type
fi

export APUS_TOOLCHAIN_PATH=/tmp/gentoo/home/amos/toolchain-sysroot
export PATH=${APUS_TOOLCHAIN_PATH}/bin:$PATH

export MSAN_OPTIONS="halt_on_error=0:report_umrs=0"

# thrift compiler has mem leak, which breaks asan build during code generation
export ASAN_OPTIONS=detect_leaks=0

case "$type" in
dall)
    configure() {
        cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_RUST=0 ../src
    }
    ;;
d)
    configure() {
        cmake \
           -DCMAKE_BUILD_TYPE=Debug \
           -DDEBUG_O_LEVEL="0" \
           -DENABLE_TESTS=1 \
           -DENABLE_RUST=0 \
           ../src
    }
    ;;
a)
    configure() {
        cmake \
           -DSANITIZE=address \
           -DENABLE_RUST=0 \
           -DCMAKE_BUILD_TYPE=Debug \
           -DDEBUG_O_LEVEL="0" \
           -DENABLE_TESTS=1 \
           ../src
    }
    ;;
m)
    configure() {
        cmake \
           -DSANITIZE=memory \
           -DENABLE_RUST=0 \
           -DCMAKE_BUILD_TYPE=Debug \
           -DDEBUG_O_LEVEL="0" \
           -DENABLE_TESTS=1 \
           ../src
    }
    ;;
u)
    configure() {
        cmake \
           -DSANITIZE=undefined \
           -DENABLE_RUST=0 \
           -DENABLE_JEMALLOC=0 \
           -DENABLE_TESTS=0 \
           -DENABLE_UTILS=0 \
           -DENABLE_SSH=0 \
           -DENABLE_RUST=0 \
           -DENABLE_AVRO=0 \
           ../src
    }
    ;;
r)
    configure() {
        cmake \
           -DENABLE_TESTS=1 \
           -DENABLE_RUST=0 \
           -DENABLE_THINLTO=0 \
           ../src
    }
    ;;
debug)
    configure() {
        cmake \
           -DENABLE_TESTS=1 \
           -DENABLE_RUST=0 \
           -DENABLE_THINLTO=0 \
           ../debug-projection
    }
    ;;
debug2)
    configure() {
        cmake \
           -DENABLE_TESTS=1 \
           -DENABLE_RUST=0 \
           -DENABLE_THINLTO=0 \
           ../debug-projection2
    }
    ;;
fix-projection-24.8)
    export APUS_TOOLCHAIN_PATH=/tmp/gentoo/home/amos/toolchain-24.8
    export PATH=${APUS_TOOLCHAIN_PATH}/bin:$PATH
    configure() {
        cmake \
           -DENABLE_TESTS=1 \
           -DENABLE_RUST=0 \
           -DENABLE_THINLTO=0 \
           ../fix-projection-24.8
    }
    ;;
*)
    echo "Usage: $0 [d|r|a]"
    exit 1
    ;;
esac

mkdir -p "$bdir"

if [ -d build ] && [ ! -h build ]; then
    echo "A real directory named 'build' should not exist. Remove it manually then proceed again."
    exit 1
fi

ln -sfT "$bdir" build

cd "$bdir"

rebuild=0
if [ -f build.ninja ]; then
    echo "Incremental build is possible."
else
    rebuild=1
fi

if [ $rebuild -eq 1 ] || [ "$(basename "$0")" = "r" ]; then
    read -p "Rebuild from scratch is required (needed). Are you sure? [Enter to continue, Ctrl-C to quit]" -n 1 -r
    echo

    find . -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +

    configure
elif [ "$(basename "$0")" = "f" ]; then
    read -p "Reconfigure?" -n 1 -r
    echo

    configure
fi

if [ "$(basename "$0")" = "bv" ]
then
    ninja -v -d keeprsp -k 0 clickhouse
else
    ninja -k 0 clickhouse
fi
