#!/usr/bin/env python3
import argparse
import getpass
import hashlib
import hmac
import os
import struct
import sys

MAGIC = b"VISORENC"
VERSION = 2
SALT_SIZE = 16
NONCE_SIZE = 12
HASH_SIZE = 32
MAX_ITERATIONS = 5_000_000
HEADER_AUTH_SIZE = 52
HEADER = "<8sIIQ16s12s32s"
HEADER_SIZE = struct.calcsize(HEADER)


PASSWORD_MAX_LEN = 510


def derive_key(password: str, salt: bytes, iterations: int) -> bytes:
    p = password.encode("ascii")
    return hashlib.pbkdf2_hmac("sha256", p, salt, iterations, dklen=64)


def rotl32(v: int, n: int) -> int:
    return ((v << n) & 0xFFFFFFFF) | (v >> (32 - n))


def quarter_round(x: list[int], a: int, b: int, c: int, d: int) -> None:
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] ^= x[a]; x[d] = rotl32(x[d], 16)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] ^= x[c]; x[b] = rotl32(x[b], 12)
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] ^= x[a]; x[d] = rotl32(x[d], 8)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] ^= x[c]; x[b] = rotl32(x[b], 7)


def chacha20_block(key: bytes, nonce: bytes, counter: int) -> bytes:
    constants = b"expand 32-byte k"
    st = list(struct.unpack("<4I", constants))
    st += list(struct.unpack("<8I", key))
    st += [counter]
    st += list(struct.unpack("<3I", nonce))
    x = st.copy()
    for _ in range(10):
        quarter_round(x, 0, 4, 8, 12)
        quarter_round(x, 1, 5, 9, 13)
        quarter_round(x, 2, 6, 10, 14)
        quarter_round(x, 3, 7, 11, 15)
        quarter_round(x, 0, 5, 10, 15)
        quarter_round(x, 1, 6, 11, 12)
        quarter_round(x, 2, 7, 8, 13)
        quarter_round(x, 3, 4, 9, 14)
    return struct.pack("<16I", *[((x[i] + st[i]) & 0xFFFFFFFF) for i in range(16)])


def crypt_stream(data: bytes, key: bytes, nonce: bytes) -> bytes:
    out = bytearray(data)
    counter = 1
    pos = 0
    while pos < len(out):
        block = chacha20_block(key, nonce, counter)
        for b in block:
            if pos >= len(out):
                break
            out[pos] ^= b
            pos += 1
        counter += 1
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Encrypt a kernel/UKI/initrd into Visor's boot-time container format."
    )
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--iterations", type=int, default=600000,
                    help="password hash iterations, default: 600000")
    args = ap.parse_args()

    if args.iterations < 1:
        ap.error("--iterations must be at least 1")
    if args.iterations > MAX_ITERATIONS:
        ap.error(f"--iterations must be at most {MAX_ITERATIONS}")

    password = getpass.getpass("Password: ")
    confirm = getpass.getpass("Confirm: ")
    if password != confirm:
        print("passwords do not match", file=sys.stderr)
        return 1
    if not password:
        print("empty passwords are not allowed", file=sys.stderr)
        return 1
    if not password.isascii():
        print("password must be ASCII only (the bootloader cannot enter "
              "non-ASCII characters)", file=sys.stderr)
        return 1
    if len(password) > PASSWORD_MAX_LEN:
        print(f"password must be at most {PASSWORD_MAX_LEN} characters "
              "(bootloader input limit)", file=sys.stderr)
        return 1

    with open(args.input, "rb") as f:
        plain = f.read()
    if not plain:
        print("input is empty", file=sys.stderr)
        return 1

    salt = os.urandom(SALT_SIZE)
    nonce = os.urandom(NONCE_SIZE)
    keys = derive_key(password, salt, args.iterations)
    enc_key = keys[:32]
    mac_key = keys[32:]
    cipher = crypt_stream(plain, enc_key, nonce)
    auth_header = struct.pack("<8sIIQ16s12s", MAGIC, VERSION, args.iterations,
                              len(plain), salt, nonce)
    tag = hmac.new(mac_key, auth_header + cipher, hashlib.sha256).digest()
    header = auth_header + tag

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(cipher)

    print(f"wrote {args.output} ({HEADER_SIZE + len(cipher)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
