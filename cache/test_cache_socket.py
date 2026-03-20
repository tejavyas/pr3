#!/usr/bin/env python3
"""Test cache command channel: connect, send request (path that will miss), read response."""
import socket
import struct
import sys

# Must match cache-student.h: cache_request_t and cache_response_t
MAX_PATH_LEN = 1024
SHM_NAME_MAX = 64

def pack_request(path, shm_name, seg_size):
    path_bytes = path.encode('utf-8')[:MAX_PATH_LEN-1].ljust(MAX_PATH_LEN, b'\0')
    shm_bytes = shm_name.encode('utf-8')[:SHM_NAME_MAX-1].ljust(SHM_NAME_MAX, b'\0')
    return path_bytes + shm_bytes + struct.pack('Q', seg_size)  # size_t

def unpack_response(data):
    # C struct: int (4) + padding (4) + size_t (8) = 16 on 64-bit
    if len(data) < 16:
        return None
    found, = struct.unpack('i', data[:4])
    file_len, = struct.unpack('Q', data[8:16])
    return (found, file_len)

SOCKET_PATH = "/tmp/pr3_cache_cmd_socket"

def main():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCKET_PATH)
    except FileNotFoundError:
        print("Cache socket not found - is simplecached running?")
        sys.exit(1)
    req = pack_request("/nonexistent/path", "/pr3_shm_0_0", 5712)
    s.sendall(req)
    resp_data = s.recv(256)
    s.close()
    r = unpack_response(resp_data)
    if r is None:
        print("Bad response")
        sys.exit(1)
    found, file_len = r
    if found != 0:
        print(f"Expected found=0 (miss), got found={found}")
        sys.exit(1)
    print("OK: cache miss returned found=0, file_len=", file_len)
    return 0

if __name__ == "__main__":
    sys.exit(main())
