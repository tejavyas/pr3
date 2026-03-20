#!/bin/bash
# Run proxy and cache tests. Best run on Linux x86_64 (e.g. course VM or Gradescope).
# On macOS the provided gfserver.o and gfclient_* are Linux ELF and won't link/run.

set -e
cd "$(dirname "$0")"

echo "=== Part 1: Build server proxy ==="
cd server
make clean 2>/dev/null || true
make
echo "Starting proxy on port 16655 (background)..."
./webproxy -p 16655 -s "https://raw.githubusercontent.com/gt-cs6200/image_data" -t 2 &
PROXY_PID=$!
sleep 2
echo "Downloading one file via gfclient_download..."
if [ -x ./gfclient_download ]; then
  ./gfclient_download -p 16655 -d test_download . < workload.txt 2>/dev/null | head -5
  ls -la test_download/master/ 2>/dev/null | head -5
else
  echo "gfclient_download not found or not executable (need Linux x86_64)"
fi
kill $PROXY_PID 2>/dev/null || true
cd ..

echo ""
echo "=== Part 2: Build cache and proxy ==="
cd cache
make clean 2>/dev/null || true
make
echo "Starting simplecached (background)..."
./simplecached -c locals.txt -t 2 &
CACHE_PID=$!
sleep 2
echo "Starting webproxy (will retry until cache is up)..."
./webproxy -p 25362 -n 4 -z 5712 -t 2 &
WP_PID=$!
sleep 3
echo "Requesting cached file via gfclient_download..."
if [ -x ./gfclient_download ]; then
  echo "/courses/ud923/filecorpus/road.jpg" | ./gfclient_download -p 25362 -d test_cache . 2>/dev/null
  ls -la test_cache/courses/ud923/filecorpus/ 2>/dev/null || true
else
  echo "gfclient_download not found (need Linux x86_64)"
fi
kill $WP_PID $CACHE_PID 2>/dev/null || true
cd ..

echo ""
echo "Done."
