#!/usr/bin/env bash

set -e
export LD_BIND_NOW=1

cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"

clickhouse=build-dev/dbms/programs/clickhouse
config_path=etc

case "$(basename "$0")" in
s)
    # numactl --membind=0 taskset -c 10 $clickhouse server --config "$config_path"/config-dev.xml "$@"
    $clickhouse server --config "$config_path"/config-dev.xml "$@"
    ;;
s2)
    $clickhouse server --config "$config_path"/config-or2.xml "$@"
    ;;
s5)
    $clickhouse server --config "$config_path"/config-s5.xml "$@"
    ;;
so)
    # numactl --membind=0 taskset -c 16 "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/build-ori/dbms/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    build-ori/dbms/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    ;;
soc)
    build-ori-clang/dbms/programs/clickhouse server --config "$config_path"/config-ori.xml "$@"
    ;;
so2)
    build-ori/dbms/programs/clickhouse server --config "$config_path"/config-dev.xml "$@"
    ;;
*)
    echo "There is no server called $0 yet."
    exit 1
    ;;
esac
