#!/usr/bin/env bash

cd $CLICKHOUSE_SRC_DIR/../..

target=$(tmux display -p '#{session_name}:#{window_name}')
b && (
    # tmux send-keys -t $target.1 spiraltest C-m
    tmux send-keys -t $target.1 s C-m
    tmux split-window -h -d -t $target.1
    tmux split-window -v -d -t $target.1
    tmux split-window -v -d -t $target.3
    tmux send-keys -t $target.2 c C-m
    tmux send-keys -t $target.3 "env MINIO_ROOT_PASSWORD=clickhouse MINIO_ROOT_USER=clickhouse minio server --address :11111 ./minio_data" C-m
    tmux select-pane -t $target.1 -D # select down
)
