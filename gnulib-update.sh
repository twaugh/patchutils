#!/bin/bash
# Script to update gnulib modules
# Run this when you want to update to newer gnulib versions

set -e
cd "$(dirname "$0")"
GNULIB_DIR=gnulib-tmp
trap 'rm -rf "$GNULIB_DIR"' EXIT

echo "Updating gnulib modules..."
git clone --depth 1 https://github.com/coreutils/gnulib.git "$GNULIB_DIR"
"$GNULIB_DIR/gnulib-tool" --update
echo "Gnulib updated. Please test and commit."
