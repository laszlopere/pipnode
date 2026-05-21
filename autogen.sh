#!/bin/sh
# Run this to generate all the initial makefiles, etc.

set -e

echo "Running autoreconf..."
autoreconf --install --force --verbose

echo ""
echo "Now run ./configure && make"
