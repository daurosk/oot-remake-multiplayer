# SPDX-License-Identifier: GPL-3.0-or-later
# The official public game's key pair (docs/official-game.md): writes a new private key file (64 hex digits,
# mode 600, never overwrites) and prints its public key for coop/protocol/official_mark.hpp kOfficialPublicKey.
# Pure Python Ed25519 (RFC 8032), checked against the RFC's test vector at start. Usage: make_key.py PATH
import hashlib, os, secrets, sys

p = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
d = (-121665 * pow(121666, p - 2, p)) % p
I = pow(2, (p - 1) // 4, p)

def recover_x(y, sign):
    x2 = (y * y - 1) * pow(d * y * y + 1, p - 2, p)
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * I % p
    if x % 2 != sign:
        x = p - x
    return x

By = 4 * pow(5, p - 2, p) % p
B = (recover_x(By, 0), By, 1, recover_x(By, 0) * By % p)

def add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    Bv = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = Bv - A, D - C, D + C, Bv + A
    return (E * F % p, G * H % p, F * G % p, E * H % p)

def mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = add(Q, P)
        P = add(P, P)
        s >>= 1
    return Q

def enc(P):
    zi = pow(P[2], p - 2, p)
    x, y = P[0] * zi % p, P[1] * zi % p
    return int.to_bytes(y | ((x & 1) << 255), 32, 'little')

def expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]

def public(seed):
    return enc(mul(expand(seed)[0], B))

def sign(seed, msg):
    a, prefix = expand(seed)
    A = enc(mul(a, B))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), 'little') % L
    R = enc(mul(r, B))
    k = int.from_bytes(hashlib.sha512(R + A + msg).digest(), 'little') % L
    return R + int.to_bytes((r + k * a) % L, 32, 'little')

# RFC 8032 7.1 TEST 1
s1 = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
assert public(s1).hex() == 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'
assert sign(s1, b'').hex() == ('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155'
                               '5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')

if __name__ == '__main__':
    out = sys.argv[1]
    if os.path.exists(out):
        sys.exit('exists: ' + out)
    seed = secrets.token_bytes(32)
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(seed.hex() + '\n')
    print('public key: ' + public(seed).hex())
