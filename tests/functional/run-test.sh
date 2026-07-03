#!/usr/bin/env bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

pytest -n auto -v -m "not smt and not rkt" "$SCRIPT_DIR" "$@"
