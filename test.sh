#!/usr/bin/env bash

# NOTE: docker should have ipv6 support
# put the following into /etc/docker/daemon.json
#
# {
#       "ipv6": true,
#       "fixed-cidr-v6": "2001:db8:3:1::/64"
# }
#

set -e

set -m   # 关键：让子进程在同一前台进程组

arch=$(dpkg --print-architecture)

if [ "$arch" = "amd64" ]
then
   docker=yandex
elif [ "$arch" = "arm64" ]
then
   docker=amosbird
else
    echo "Only support arch = amd64|arm64. Given $arch."
    exit 1
fi

mkdir -p /tmp/test_output /tmp/server_log
chmod -R 777 /tmp/test_output /tmp/server_log

case "$(basename "$0")" in
ub)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/clickhouse-debs:/package_folder --volume=/tmp/test_output:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server -e SKIP_TESTS_OPTION="--skip 00281 capnproto avx2 query_profiler" -e ADDITIONAL_OPTIONS="--hung-check" "$docker"/clickhouse-stateless-test
    ;;
rel)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/clickhouse-debs:/package_folder --volume=/tmp/test_output:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server -e SKIP_TESTS_OPTION="--skip avx2" -e ADDITIONAL_OPTIONS="--hung-check" "$docker"/clickhouse-stateless-test
    ;;
stress)
    docker run --rm --ulimit nofile=1000000:1000000 --volume=/data/clickhouse-debs:/package_folder --volume=/tmp/test_output:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server "$docker"/clickhouse-stress-test
    ;;
uni)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/ClickHouse/build/unit_tests_dbms:/unit_tests_dbms --volume=/tmp/test_output:/test_output "$docker"/clickhouse-unit-test
    ;;
pf)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/clickhouse-debs:/package_folder --volume=/tmp/test_output:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server --volume=/data/clickhouse-testdata:/var/lib/clickhouse -e DOWNLOAD_DATASETS=0 -e TESTS_TO_RUN='--recursive --input-files /usr/share/clickhouse-test/performance/' "$docker"/clickhouse-performance-test
    ;;
pf1)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/clickhouse-debs:/package_folder --volume=/tmp/test_output1:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server --volume=/data/clickhouse-testdata:/var/lib/clickhouse -e DOWNLOAD_DATASETS=0 -e TESTS_TO_RUN='--input-files /usr/share/clickhouse-test/performance/website.xml --query-indexes 67 68 69 70' "$docker"/clickhouse-performance-test
    ;;
pf2)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/chorigin-debs:/package_folder --volume=/tmp/test_output2:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server --volume=/data/clickhouse-testdata:/var/lib/clickhouse -e DOWNLOAD_DATASETS=0 -e TESTS_TO_RUN='--input-files /usr/share/clickhouse-test/performance/general_purpose_hashes.xml --query-indexes 20' "$docker"/clickhouse-performance-test
    ;;
pf3)
    docker run --rm -t --ulimit nofile=1000000:1000000 --volume=/data/pfdebs:/package_folder --volume=/tmp/test_output3:/test_output --volume=/tmp/server_log:/var/log/clickhouse-server --volume=/data/clickhouse-testdata:/var/lib/clickhouse -e DOWNLOAD_DATASETS=0 -e TESTS_TO_RUN='--input-files /usr/share/clickhouse-test/performance/' "$docker"/clickhouse-performance-test
    ;;
tq)
    cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/src/tests && ./clickhouse-test --no-random-settings --no-stateful --shard --zookeeper "$@"
    ;;
ts)
    cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/src/tests && ./clickhouse-test --no-random-settings --no-stateless --shard --zookeeper "$@"
    ;;
tp)
    (cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/src/tests/performance && scripts/perf.py --host=localhost --port=9000 --runs=1 "$@")
    ;;
tp1)
    (cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/src/tests/performance && ../../docker/test/performance-comparison/perf.py --host=localhost --port=9001 --runs=1 "$@")
    ;;
tbs)
    clickhouse benchmark --max_threads 1 <<< $@
    ;;
tbs1)
    clickhouse benchmark --max_threads 1 --port 9001 <<< $@
    ;;
tb)
    clickhouse benchmark --port 19000 <<< $@
    ;;
tb1)
    clickhouse benchmark --port 19001 <<< $@
    ;;
tb2)
    clickhouse benchmark --port 19000 --port 19001 <<< $@
    ;;
*)
    echo "There is no test for this variant yet."
    exit 1
    ;;
esac
