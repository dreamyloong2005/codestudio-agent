#!/usr/bin/env sh
# User-level package installer. Requires Python 3.10+; never changes PATH.
set -eu
installer_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$installer_dir/install.py" "$@"
