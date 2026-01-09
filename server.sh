#!/usr/bin/env bash

set -e
export LD_BIND_NOW=1

ulimit -c 0

export ASAN_SYMBOLIZER_PATH=/tmp/gentoo/home/amos/toolchain-sysroot/bin/llvm-symbolizer
export ASAN_OPTIONS=${ASAN_OPTIONS}:detect_odr_violation=1:symbolize=1:verbosity=0
export LSAN_OPTIONS=${LSAN_OPTIONS}:suppressions=script/lsan_suppressions.txt

# export LIBUNWIND_PRINT_DWARF=1
# export LIBUNWIND_PRINT_UNWINDING=1
# export LIBUNWIND_PRINT_APIS=1
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

export LIBHDFS3_CONF=etc/hdfs-client.xml

clickhouse_server="build/programs/clickhouse server"
# clickhouse_server="./clickhouse-base server"
# clickhouse_server="./clickhouse-opt server"
# clickhouse_server="./clickhouse-opt-query-cache server"
# clickhouse_server="./clickhouse-opt-order-by-limit server"
config_path=etc

# export THREAD_FUZZER_CPU_TIME_PERIOD_US=1000
# export THREAD_FUZZER_SLEEP_PROBABILITY=0.1
# export THREAD_FUZZER_SLEEP_TIME_US=100000

# export THREAD_FUZZER_pthread_mutex_lock_BEFORE_MIGRATE_PROBABILITY=1
# export THREAD_FUZZER_pthread_mutex_lock_AFTER_MIGRATE_PROBABILITY=1
# export THREAD_FUZZER_pthread_mutex_unlock_BEFORE_MIGRATE_PROBABILITY=1
# export THREAD_FUZZER_pthread_mutex_unlock_AFTER_MIGRATE_PROBABILITY=1

# export THREAD_FUZZER_pthread_mutex_lock_BEFORE_SLEEP_PROBABILITY=0.001
# export THREAD_FUZZER_pthread_mutex_lock_AFTER_SLEEP_PROBABILITY=0.001
# export THREAD_FUZZER_pthread_mutex_unlock_BEFORE_SLEEP_PROBABILITY=0.001
# export THREAD_FUZZER_pthread_mutex_unlock_AFTER_SLEEP_PROBABILITY=0.001
# export THREAD_FUZZER_pthread_mutex_lock_BEFORE_SLEEP_TIME_US=10000
# export THREAD_FUZZER_pthread_mutex_lock_AFTER_SLEEP_TIME_US=10000
# export THREAD_FUZZER_pthread_mutex_unlock_BEFORE_SLEEP_TIME_US=10000
# export THREAD_FUZZER_pthread_mutex_unlock_AFTER_SLEEP_TIME_US=10000

case "$(basename "$0")" in
s)
    # numactl --membind=0 taskset -c 10 $clickhouse server --config "$config_path"/config-dev.xml "$@"
    $clickhouse_server --config "$config_path"/config-dev.xml "$@"
    ;;
sd)
    # numactl --membind=0 taskset -c 10 $clickhouse_server --config "$config_path"/config-dev.xml "$@"
    lldb $clickhouse_server -- --config "$config_path"/config-dev.xml "$@"
    ;;
s2)
    $clickhouse_server --config "$config_path"/config-dev2.xml "$@"
    ;;
s5)
    $clickhouse_server --config "$config_path"/config-s5.xml "$@"
    ;;
so)
    # numactl --membind=0 taskset -c 16 "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/build-ori/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    build-ori/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    ;;
soc)
    build-ori-clang/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    ;;
so2)
    build-ori/programs/clickhouse server --config "$config_path"/config-dev.xml "$@"
    ;;
*)
    echo "There is no server called $0 yet."
    exit 1
    ;;
esac
