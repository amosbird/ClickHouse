#!/usr/bin/env bash

sudo docker run --net=host -it --rm --privileged --volume=$PWD/build/programs/clickhouse:/clickhouse --volume=$PWD/build/programs/clickhouse:/usr/share/clickhouse_fresh --volume=/dev/null:/usr/bin/clickhouse-odbc-bridge --volume=/dev/null:/usr/share/clickhouse-odbc-bridge_fresh --volume=$PWD/src/programs/server:/clickhouse-config --volume=$PWD/src:/ClickHouse --volume=$PWD/src/docker/test/integration/runner/compose:/compose:ro --volume=ch-vol:/var/lib/docker -e PYTEST_ADDOPTS='-vvv -s --pdb test_jbod_balancer' --name ch clickhouse/integration-tests-runner pytest
