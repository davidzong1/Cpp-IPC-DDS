#!/bin/bash
set -u
ROOT="$1"; shift
REQUIRED_TOPOLOGY="a.csv,b.csv"
declare -a RESULTS=(); declare -a EXPECTED_FAILS=()
printf '%s\trc=%s\tok=%s\t%s\tmissing=%s\n' "$@" >> /dev/null
