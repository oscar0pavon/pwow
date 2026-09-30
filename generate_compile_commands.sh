#!/bin/sh
make compile_commands | awk '{ if (sub(/\\$/, "")) printf "%s ", $0; else print $0 }' | grep -w cc | \
	jq -nR '[inputs | . as $cmd | {directory: "/root/pwow", command: $cmd, file: [scan("[^ ]+\\.c")][]}]' \
	> compile_commands.json
