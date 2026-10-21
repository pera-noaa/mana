#!/bin/sh
# threadjoin.sh BIN: threadjoin under plain DMTCP (BIN: its dmtcp_* tools), checkpointed, killed and restarted.
B=$1; P=7781; D=$(mktemp -d); cd $D || exit 2
gcc -O0 -pthread -o threadjoin $GITHUB_WORKSPACE/ci543/threadjoin.c || exit 2
touch stop; ./threadjoin || exit 2; rm stop  # native
$B/dmtcp_coordinator --daemon -p $P -q
$B/dmtcp_launch -p $P ./threadjoin > launch.out 2>&1 &
sleep 3; $B/dmtcp_command -p $P -bc; sleep 2; ls ckpt_*.dmtcp || exit 2
$B/dmtcp_command -p $P -k; sleep 2
$B/dmtcp_restart -p $P ckpt_*.dmtcp > restart.out 2>&1 &
R=$!; sleep 3; touch stop; wait $R; rc=$?
cat launch.out restart.out; $B/dmtcp_command -p $P -q 2>/dev/null
echo "THREADJOIN rc=$rc"; exit $rc
