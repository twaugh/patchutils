#!/bin/bash
# Script to update gnulib modules
# Run this when you want to update to newer gnulib versions

echo "Updating gnulib modules..."
git clone --depth 1 https://github.com/coreutils/gnulib.git /tmp/gnulib-update
/tmp/gnulib-update/gnulib-tool --update
rm -rf /tmp/gnulib-update
echo "Gnulib updated. Please test and commit."
