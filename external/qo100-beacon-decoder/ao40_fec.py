#!/usr/bin/env python3
"""
AO-40 FEC decode zinciri (saf Python/numpy):
  distributed sync -> deinterleave -> Viterbi(K=7,r=1/2) -> CCSDS descramble
  -> Reed-Solomon(160,128) x2 (GF(2^8) poly 0x187, fcr=112, prim=11)

Referans: gr-satellites (Daniel Estevez) + libfec (Phil Karn KA9Q).
"""
import numpy as np

SYNCWORD = '11111110000111011110010110010010000001000100110001011101011011000'
SYNC = np.array([int(c) for c in SYNCWORD], dtype=np.uint8)
STEP = 80
FRAME_SYMS = 65 * 80          # 5200
CONV_SYMS = 5132              # deinterleave sonrasi Viterbi girdisi
CONV_BITS = CONV_SYMS // 2 - 6  # 2560 bit = 320 byte

# ---------------------------------------------------------------- sync arama
def find_fec_frames(bits, threshold=8):
    """Hard bit dizisinde dagitik sync ara. bits: 0/1 dizisi.
    Dondurur: frame baslangic indeksleri (her frame 5200 bit)."""
    n = len(bits)
    hits = []
    if n < FRAME_SYMS:
        return hits
    # her i icin sum_j bits[i+80j] == SYNC[j]
    b = bits.astype(np.int16)
    limit = n - FRAME_SYMS + 1
    score = np.zeros(limit, dtype=np.int16)
    for j in range(65):
        seg = b[j * STEP: j * STEP + limit]
        score += (seg == SYNC[j])
    cand = np.where(score >= 65 - threshold)[0]
    # yakin tekrarlari ele (ayni frame'e birden fazla vurus)
    last = -FRAME_SYMS
    for c in cand:
        if c - last >= FRAME_SYMS // 2:
            hits.append(int(c))
            last = c
    return hits


# -------------------------------------------------------------- deinterleave
def deinterleave(frame_bits):
    """5200 bit -> 5132 bit (sync sutunu atilir).
    gr-satellites matrix_deinterleaver_soft(80, 65, 5132, 65) esdegeri."""
    k = np.arange(FRAME_SYMS)
    out = frame_bits[STEP * (k % 65) + k // 65]
    return out[65:65 + CONV_SYMS]


# ------------------------------------------------------------------- viterbi
# GR cc_decoder polinomlari [79, -109]: G1=0b1001111, G2=0b1101101 (ters cikis)
G1, G2 = 79, 109
K = 7
NSTATES = 64

def _enc_outputs():
    """Her (state, girisbit) icin iki cikis biti tablosu.
    state = son 6 giris biti (yeni bit MSB'ye girer - konvansiyon testle dogrulanir)."""
    out = np.zeros((2, NSTATES, 2), dtype=np.uint8)
    for s in range(NSTATES):
        for bit in (0, 1):
            reg = (bit << 6) | s          # 7 bitlik pencere
            o1 = bin(reg & G1).count('1') & 1
            o2 = (bin(reg & G2).count('1') & 1) ^ 1   # ters
            out[bit, s, 0] = o1
            out[bit, s, 1] = o2
    return out

ENC = _enc_outputs()

def conv_encode(bits):
    """Terminated kodlama (6 sifir kuyruk). Dogrulama icin.
    Konvansiyon (golden vektorle dogrulandi): yeni bit LSB'den girer,
    cikis1 = parity(reg&79), cikis2 = parity(reg&109) tersli."""
    reg = 0
    out = []
    for b in list(bits) + [0] * 6:
        reg = ((reg << 1) | int(b)) & 0x7F
        out.append(bin(reg & G1).count('1') & 1)
        out.append((bin(reg & G2).count('1') & 1) ^ 1)
    return np.array(out[:CONV_SYMS], dtype=np.uint8)

def viterbi_decode(syms):
    """Hard-decision Viterbi, terminated. syms: 5132 bit (0/1) -> 2560 bit.
    State = son 6 giris biti (en yeni LSB)."""
    nsteps = len(syms) // 2
    INF = 1 << 20
    s_arr = np.arange(NSTATES)
    # input b: reg = (s<<1)|b (7 bit), yeni state = reg & 63
    regs0 = (s_arr << 1)          # b=0
    regs1 = (s_arr << 1) | 1      # b=1
    ns0 = regs0 & 63
    ns1 = regs1 & 63
    def par(v, mask):
        return np.array([bin(int(x) & mask).count('1') & 1 for x in v], dtype=np.uint8)
    o01, o02 = par(regs0, G1), par(regs0, G2) ^ 1
    o11, o12 = par(regs1, G1), par(regs1, G2) ^ 1
    pm = np.full(NSTATES, INF, dtype=np.int32)
    pm[0] = 0
    prev_state = np.zeros((nsteps, NSTATES), dtype=np.int8)
    prev_bit = np.zeros((nsteps, NSTATES), dtype=np.uint8)
    for t in range(nsteps):
        r1, r2 = int(syms[2 * t]), int(syms[2 * t + 1])
        c0 = pm + (o01 != r1) + (o02 != r2)
        c1 = pm + (o11 != r1) + (o12 != r2)
        new_pm = np.full(NSTATES, INF, dtype=np.int32)
        for b, cand, nsmap in ((0, c0, ns0), (1, c1, ns1)):
            for s in range(NSTATES):
                ns = nsmap[s]
                if cand[s] < new_pm[ns]:
                    new_pm[ns] = cand[s]
                    prev_state[t, ns] = s
                    prev_bit[t, ns] = b
        pm = new_pm
    state = 0
    bits = np.zeros(nsteps, dtype=np.uint8)
    for t in range(nsteps - 1, -1, -1):
        bits[t] = prev_bit[t, state]
        state = prev_state[t, state]
    return bits[:CONV_BITS]


# --------------------------------------------------------------- descrambler
def ccsds_sequence(nbytes):
    """CCSDS randomizer: x^8+x^7+x^5+x^3+1, hepsi-1 baslangic.
    Ilk byte'lar: FF 48 0E C0 9A 0D 70 BC ..."""
    reg = 0xFF
    out = bytearray()
    for _ in range(nbytes):
        byte = 0
        for _ in range(8):
            bit = reg & 1
            byte = (byte << 1) | bit
            fb = ((reg >> 0) ^ (reg >> 3) ^ (reg >> 5) ^ (reg >> 7)) & 1
            reg = (reg >> 1) | (fb << 7)
        out.append(byte)
    return np.frombuffer(bytes(out), dtype=np.uint8)


# ------------------------------------------------------------- reed-solomon
GFPOLY, FCR, PRIM, NROOTS, NN = 0x187, 112, 11, 32, 255
IPRIM = 116   # PRIM * IPRIM = 1 mod 255

_exp = np.zeros(256, dtype=np.int32)
_log = np.zeros(256, dtype=np.int32)
def _init_gf():
    x = 1
    for i in range(255):
        _exp[i] = x
        _log[x] = i
        x <<= 1
        if x & 0x100:
            x ^= GFPOLY
    _exp[255] = _exp[0]
_init_gf()

def _gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return int(_exp[(_log[a] + _log[b]) % 255])

def rs_decode(codeword):
    """255 byte codeword (bastan pad'li), yerinde duzeltir.
    Dondurur: duzeltilen hata sayisi, ya da -1 (basarisiz)."""
    data = list(codeword)
    # sendromlar: S_i = C(alpha^(PRIM*(FCR+i)))
    synd = []
    for i in range(NROOTS):
        root = (PRIM * (FCR + i)) % 255
        s = 0
        for c in data:
            s = _gmul(s, _exp[root]) ^ c
        synd.append(s)
    if max(synd) == 0:
        return 0, bytes(data)
    # Berlekamp-Massey
    C = [1] + [0] * NROOTS
    B = [1] + [0] * NROOTS
    L, m, b = 0, 1, 1
    for n in range(NROOTS):
        d = synd[n]
        for i in range(1, L + 1):
            d ^= _gmul(C[i], synd[n - i])
        if d == 0:
            m += 1
        elif 2 * L <= n:
            T = C[:]
            coef = _gmul(d, _exp[(255 - _log[b]) % 255])
            for i in range(NROOTS - m + 1):
                C[i + m] ^= _gmul(coef, B[i])
            L, B, b, m = n + 1 - L, T, d, 1
        else:
            coef = _gmul(d, _exp[(255 - _log[b]) % 255])
            for i in range(NROOTS - m + 1):
                C[i + m] ^= _gmul(coef, B[i])
            m += 1
    if L > NROOTS // 2:
        return -1, bytes(data)
    # Chien: hata konumlari. C(x) koku x = beta^(-j) (beta = alpha^PRIM)
    err_pos = []
    for j in range(255):
        # x = alpha^(-PRIM*j)
        xlog = (255 - (PRIM * j) % 255) % 255
        v = 0
        for i in range(L + 1):
            if C[i]:
                v ^= _exp[(_log[C[i]] + i * xlog) % 255]
        if v == 0:
            err_pos.append(254 - j)   # konum: soldan index (deneysel dogrulanir)
    if len(err_pos) != L:
        return -1, bytes(data)
    # Forney
    # omega(x) = S(x)*C(x) mod x^NROOTS
    S_poly = synd[:]
    omega = [0] * NROOTS
    for i in range(NROOTS):
        acc = 0
        for k in range(i + 1):
            if k <= L and C[k]:
                acc ^= _gmul(C[k], S_poly[i - k])
        omega[i] = acc
    for pos in err_pos:
        j = 254 - pos
        xinv_log = (PRIM * j) % 255          # log of beta^j = x^-1... deneysel
        # X_k = beta^j ; err = X^(1-FCR) * omega(X^-1) / C'(X^-1)
        Xlog = (255 - xinv_log) % 255        # log of X^-1 = alpha^(-PRIM*j)
        num = 0
        for i in range(NROOTS):
            if omega[i]:
                num ^= _exp[(_log[omega[i]] + i * Xlog) % 255]
        den = 0
        for i in range(1, L + 1, 2):
            if C[i]:
                den ^= _exp[(_log[C[i]] + (i - 1) * Xlog) % 255]
        if den == 0:
            return -1, bytes(data)
        # X^(FCR-1) carpani: err = num/den * X^(1-FCR) -> X = beta^j
        Xj_log = (PRIM * j) % 255
        mag = _exp[(_log[num] - _log[den] + (1 - FCR) * Xj_log) % 255] if num else 0
        data[pos] ^= mag
    return L, bytes(data)


def rs_decode_frame(frame320):
    """320 byte -> 256 byte (2 interleaved RS codeword). None = basarisiz."""
    out = bytearray(256)
    total_err = 0
    for j in range(2):
        cw = bytes(95) + bytes(frame320[j::2])   # 95 pad + 160
        nerr, fixed = rs_decode(cw)
        if nerr < 0:
            return None, -1
        total_err += nerr
        for k in range(128):
            out[j + 2 * k] = fixed[95 + k]
    return bytes(out), total_err


# ------------------------------------------------------------------ tam zincir
def decode_fec_frame(frame_bits):
    """5200 bitlik frame -> (256 byte payload, rs_hata) ya da (None, -1)."""
    conv = deinterleave(frame_bits)
    bits = viterbi_decode(conv)
    by = np.packbits(bits)
    descr = by ^ ccsds_sequence(len(by))
    return rs_decode_frame(descr)
