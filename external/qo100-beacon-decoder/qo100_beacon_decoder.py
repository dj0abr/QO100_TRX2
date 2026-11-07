#!/usr/bin/env python3
"""
QO-100 NB orta beacon (10489.750 MHz, 400 bps BPSK, bifaz kodlu) WAV dekoderi
+ P3-D cerceve ayristirici (sync, 512 byte blok, CRC) + binary/FEC gorsellestirme.

Kullanim:
    python3 qo100_beacon_decoder.py kayit.wav

Beklenen kayit: WebSDR'den USB modunda, dial ~10489.748 kHz civarinda alinmis
mono WAV. Beacon tonu ses bandinda 1000-2500 Hz araligina dusmeli.
Gereksinimler: numpy, scipy, matplotlib

Zincir:  WAV -> Hilbert (analitik sinyal) -> tasiyici bulma (spektrum agirlik
merkezi + kare alma ile ince ayar) -> faz takibi -> 800 chip/s ornekleme ->
bifaz/Manchester + diferansiyel cozme -> sync arama -> CRC -> blok ayristirma.

Gerekli kutuphaneler;

pip3 install numpy scipy matplotlib

python3 qo100_beacon_decoder.py ~/Downloads/is0grb_websdr_recording_2026-08-05T14_13_24Z_10489748.3kHz.wav
python3 qo100_beacon_decoder.py sample.wav

sample
------
leventd telsiz #python3 qo100_beacon_decoder.py ~/Downloads/is0grb_websdr_recording_2026-08-05T14_13_24Z_10489748.3kHz.wav
/Users/leventd/telsiz/qo100_beacon_decoder.py:30: WavFileWarning: Reached EOF prematurely; finished at 194348 bytes, expected 194356 bytes from header.
  fs, x = wavfile.read(path)
tasiyici: 1732.53 Hz
chip sayisi: 9716, goz kalitesi: 5.00 (>2 iyi)
ASCII orani: 0.56
------------------------------------------------------------
         All users on QO-100 are encouraged to monitor                   this frequency, but keep it clear for emergency traffic!        .}.PPPP...

Kayit alma
----------
10489748.50 Khz
USB 2.7
2.75 kHz @ -6dB; 3.21 kHz @ -60dB
DSP Noise Reduction: Disabled
Audio Buffer: +250 ms (default)
Audio AGC: Auto

Mute: Off
Squelch: Off
Autonotch: Off
High Boost: Off
Signal Strength Plot: None

Volume: Full

Cerceve formati (AMSAT P3-D tlmspec, Release 1.8)
-------------------------------------------------
[~130 byte 0x50 'P' bosluk][SYNC 39 15 ED 30][512 byte veri][2 byte CRC]

- CRC: x^16 + x^12 + x^5 + 1 (AMSAT CYC2 / CCITT), init 0xFFFF, MSB-first.
  Dogru CRC'li 514 byte uzerinden hesaplanan CRC = 0 cikar.
- Blok tipi = ilk byte: A=telemetri (128 analog + 128 digital kanal),
  K/L/M/N=mesaj bloklari, E=olay, D=dosya, X=yazilim.
- QO-100 beacon'i metin bloklariyla donusumlu olarak AO-40 FEC cerceveleri
  yayinlar (scrambling + convolutional K=7 r=1/2 + Reed-Solomon + interleave).
  Bunlarin sync'i "distributed" oldugu icin duz aramayla bulunmaz; bu script
  onlari fec_XX.bin olarak kaydeder ve isi haritasinda gosterir.
  Spec: https://amsat-dl.org/wp-content/uploads/2019/01/tlmspec.txt
  FEC:  https://amsat.org/articles/g3ruh/125.html
"""
import contextlib
import io
import os
import re
import sys
import numpy as np
from scipy.io import wavfile
from scipy import signal

try:
    import ao40_fec                     # FEC decode zinciri (ayni klasorde)
except ImportError:
    ao40_fec = None

SYNC = 0x3915ED30
BLOCK_BYTES = 512 + 2          # veri + CRC
FRAME_BITS = 32 + BLOCK_BYTES * 8


# ---------------------------------------------------------------- DSP katmani
def wav_to_chips(path):
    """WAV -> 800 chip/s sert karar dizisi (bifaz chipleri).
    Ayrica (fs, x) dondurur ki spektrum gorseli cizilebilsin."""
    fs, x = wavfile.read(path)
    if x.ndim > 1:
        x = x[:, 0]
    x = x.astype(np.float64) / np.max(np.abs(x))
    xa = signal.hilbert(x)
    n = np.arange(len(xa))

    # 1) Kaba tasiyici: 1000-2500 Hz bandinin spektral agirlik merkezi
    f, Pxx = signal.welch(x, fs, nperseg=8192)
    band = (f > 1000) & (f < 2500)
    fc = np.sum(f[band] * Pxx[band]) / np.sum(Pxx[band])

    # 2) Ince ayar: BPSK'da sinyalin karesi 2*fc'de tek ton verir
    lp = signal.firwin(301, 900 / (fs / 2))
    bb = signal.filtfilt(lp, [1], xa * np.exp(-2j * np.pi * fc * n / fs))
    sq = bb ** 2
    F = np.fft.fft(sq * np.hanning(len(sq)))
    fax = np.fft.fftfreq(len(sq), 1 / fs)
    fc += fax[np.argmax(np.abs(F) * (np.abs(fax) < 50))] / 2
    print(f"tasiyici: {fc:.2f} Hz")

    # 3) Tam asagi cevirme + faz takibi (kare sinyalin fazinin yarisi)
    bb = signal.filtfilt(lp, [1], xa * np.exp(-2j * np.pi * fc * n / fs))
    sq = signal.filtfilt(signal.firwin(501, 20 / (fs / 2)), [1], bb ** 2)
    I = np.real(bb * np.exp(-0.5j * np.unwrap(np.angle(sq))))

    # 4) 800 chip/s senkronizasyon (bifaz: her 400bps bit = 2 chip)
    sps = fs / 800.0
    mf = np.ones(int(round(sps))) / sps
    Im = signal.filtfilt(mf, [1], I)
    off = max(range(int(sps)),
              key=lambda o: np.mean(np.abs(Im[np.arange(o, len(Im), sps).astype(int)])))
    idx = np.arange(off, len(Im) - 1, sps).round().astype(int)
    soft = Im[idx]
    snr_eye = np.mean(np.abs(soft)) / np.std(np.abs(soft))
    print(f"chip sayisi: {len(soft)}, goz kalitesi: {snr_eye:.2f} (>2 iyi)")
    return (soft > 0).astype(np.uint8), fs, x


def chips_to_bits(chips, align):
    """Bifaz cozme + diferansiyel: chip cifti -> 400bps bit."""
    c = chips[align:]
    pairs = c[: len(c) // 2 * 2].reshape(-1, 2)
    return np.bitwise_xor(pairs[1:, 0], pairs[:-1, 0])


# ------------------------------------------------------------ cerceve katmani
def crc16_cyc2(data):
    """AMSAT CYC2: x^16+x^12+x^5+1, init 0xFFFF, MSB-first."""
    crc = 0xFFFF
    for byte in data:
        crc ^= int(byte) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bits_to_bytes(bits):
    return np.packbits(bits[: len(bits) // 8 * 8])


def find_blocks(bits):
    """Bit dizisinde sync ara (byte hizasi gerekmez), 514 byte cek, CRC dogrula."""
    pat = 2 * np.array([(SYNC >> (31 - i)) & 1 for i in range(32)], dtype=np.int16) - 1
    conv = np.convolve(2 * bits.astype(np.int16) - 1, pat[::-1])
    hits = np.where(conv >= 32)[0] - 31   # 32/32 bit tam eslesme
    blocks = []
    for h in hits:
        s, e = h + 32, h + FRAME_BITS
        if s < 0 or e > len(bits):
            continue
        blk = bits_to_bytes(bits[s:e])
        blocks.append((h, blk[:512], crc16_cyc2(blk) == 0))
    return blocks


def decode_fec_frames(bits):
    """AO-40 FEC cercevelerini bul ve coz (ao40_fec.py gerekli).
    Dondurur: [(bit_pozisyonu, 256 byte payload | None, rs_hata)]"""
    if ao40_fec is None:
        print("ao40_fec.py bulunamadi — FEC bloklari cozulmeden atlaniyor")
        return []
    results = []
    for stream in (bits, bits ^ 1):     # her iki polarite
        hits = ao40_fec.find_fec_frames(stream)
        if hits:
            for h in hits:
                payload, nerr = ao40_fec.decode_fec_frame(stream[h:h + 5200])
                results.append((h, payload, nerr))
            break
    return results


# ------------------------------------------------------------------- raporlama
def parse_utc(kind, data):
    """A-tipi blogun basligindan UTC cek (spec: 'A' + tarih + saat).
    Sadece baslik bolgesine bakilir; bulten metnindeki tarihler sayilmaz.
    Diger tum blok tipleri icin N/A."""
    if data is None or chr(data[0]) != 'A':
        return 'N/A'
    head = ''.join(chr(v) if 32 <= v < 127 else ' ' for v in data[:40])
    md = re.search(r'\d{4}-\d{2}-\d{2}', head)
    mt = re.search(r'\d{2}:\d{2}:\d{2}', head)
    if md and mt:
        return f'{md.group()} {mt.group()}'
    return 'N/A (A blogu ama baslik cozulemedi)'


def report(blocks, fec, total_dur):
    """Kronolojik dokum: bloklar yayin sirasiyla (bir duz, bir FEC).
    t = kayit basindan itibaren saniye (bit_pozisyonu / 400 bps).
    Bloklar arasi beklenenden buyuk bosluklar acikca isaretlenir."""
    UNCODED_DUR = (32 + BLOCK_BYTES * 8) / 400.0   # sync+veri+crc = 10.4 s
    FEC_DUR = 5200 / 400.0                          # 13.0 s
    GAP_TOL = 3.0                                   # normal blok arasi ~1-1.3 s

    timeline = []
    for pos, blk, ok in blocks:
        timeline.append((pos, 'DUZ', blk, ok, None))
    for pos, payload, nerr in fec:
        timeline.append((pos, 'FEC', payload, payload is not None, nerr))
    timeline.sort(key=lambda e: e[0])

    def typ(data):
        if data is None:
            return '?'
        c = data[0]
        return chr(c) if 32 <= c < 127 else f'0x{c:02X}'

    def dur_of(kind):
        return UNCODED_DUR if kind == 'DUZ' else FEC_DUR

    # bosluk tespiti: kayit basi / bloklar arasi / kayit sonu
    gaps = []                       # (nereden_onceki_index, t0, t1)
    if timeline and timeline[0][0] / 400.0 > GAP_TOL:
        gaps.append((-1, 0.0, timeline[0][0] / 400.0))
    for i in range(len(timeline) - 1):
        end_i = timeline[i][0] / 400.0 + dur_of(timeline[i][1])
        start_n = timeline[i + 1][0] / 400.0
        if start_n - end_i > GAP_TOL:
            gaps.append((i, end_i, start_n))
    if timeline:
        end_last = timeline[-1][0] / 400.0 + dur_of(timeline[-1][1])
        if total_dur - end_last > GAP_TOL:
            gaps.append((len(timeline) - 1, end_last, total_dur))
    gap_after = {g[0]: g for g in gaps}

    def gap_line(t0, t1):
        return (f"    [!] t={t0:.1f}s-{t1:.1f}s arasi ({t1-t0:.1f}s) okunamadi "
                f"- yarim/bozuk blok ya da kayit siniri")

    # ozet tablo
    print("\n" + "=" * 78)
    print("KAYIT DOKUMU - kronolojik  (t = kayit basindan saniye, 400 bps, "
          f"kayit {total_dur:.1f}s)")
    print("=" * 78)
    print(f"{'#':>2}  {'t_bas':>7}  {'t_son':>7}  {'tur':<4} {'tip':<4} "
          f"{'boyut':>5}  {'UTC':<5} dogrulama")
    print("-" * 78)
    prev_plain = {}
    notes = {}
    if -1 in gap_after:
        print(gap_line(gap_after[-1][1], gap_after[-1][2]))
    for i, (pos, kind, data, ok, nerr) in enumerate(timeline):
        t0 = pos / 400.0
        utc = parse_utc(kind, data)
        if kind == 'DUZ':
            ver = 'CRC OK' if ok else 'CRC HATALI'
            size = '512B'
            if ok:
                prev_plain[typ(data)] = bytes(data[:256])
        else:
            ver = f'RS OK ({nerr} duzeltme)' if ok else 'RS BASARISIZ'
            size = '256B'
            if ok and typ(data) in prev_plain:
                same = bytes(data) == prev_plain[typ(data)]
                notes[i] = ('onceki ' + typ(data) + " blogunun ilk yarisi"
                            + (' (birebir ayni)' if same else ' (FARKLI!)'))
        print(f"{i:>2}  {t0:>6.1f}s  {t0+dur_of(kind):>6.1f}s  {kind:<4} "
              f"{typ(data):<4} {size:>5}  {utc:<5} {ver}"
              + ('  <- ' + notes[i] if i in notes else ''))
        if i in gap_after:
            print(gap_line(gap_after[i][1], gap_after[i][2]))
    print("-" * 78)
    nplain = sum(1 for e in timeline if e[1] == 'DUZ')
    nfec = len(timeline) - nplain
    print(f"toplam: {nplain} duz + {nfec} FEC blok, {len(gaps)} okunamayan aralik. "
          f"Yayin duzeni: duz -> FEC -> duz -> FEC ...")

    # detayli icerik, ayni kronolojik sirayla
    if -1 in gap_after:
        print("\n" + "=" * 78)
        print(gap_line(gap_after[-1][1], gap_after[-1][2]).strip())
        print("=" * 78)
    for i, (pos, kind, data, ok, nerr) in enumerate(timeline):
        t0 = pos / 400.0
        head = (f"[{i}] t={t0:.1f}s  {kind}  tip='{typ(data)}'  "
                f"UTC: {parse_utc(kind, data)}  "
                + ('CRC OK' if kind == 'DUZ' and ok else
                   'CRC HATALI' if kind == 'DUZ' else
                   f'RS OK, {nerr} byte duzeltildi' if ok else 'RS BASARISIZ'))
        print("\n" + "=" * 78)
        print(head + (('  |  ' + notes[i]) if i in notes else ''))
        print("-" * 78)
        if data is None:
            print("  (cozulemedi)")
        else:
            txt = ''.join(chr(v) if 32 <= v < 127 else '.' for v in data)
            for o in range(0, len(txt), 64):
                print("  " + txt[o:o + 64])
        print("=" * 78)
        if i in gap_after:
            print("\n" + "=" * 78)
            print(gap_line(gap_after[i][1], gap_after[i][2]).strip())
            print("=" * 78)


def plot_spectrum(fs, x, png_path):
    """Kaydin guc spektrumu + spektrogrami: bifaz BPSK'nin cift tumsegi ve
    1500 Hz'deki bastirilmis tasiyici gorunur. README'deki gorselin aynisi."""
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib yok — spektrum gorseli atlandi")
        return
    fig, axes = plt.subplots(2, 1, figsize=(12, 7))
    f, Pxx = signal.welch(x, fs, nperseg=4096)
    axes[0].semilogy(f, Pxx, lw=0.8)
    axes[0].set_xlim(0, 4000)
    axes[0].set_xlabel('Audio frequency (Hz)')
    axes[0].set_ylabel('Power')
    axes[0].set_title('QO-100 PSK beacon — power spectrum: twin humps of '
                      'biphase BPSK, null at 1500 Hz carrier')
    axes[0].grid(True, alpha=0.3)
    axes[0].annotate('carrier (suppressed)', xy=(1500, 2e-7), xytext=(2300, 3e-6),
                     arrowprops=dict(arrowstyle='->'), fontsize=9)
    seg = x[:int(min(40, len(x) / fs) * fs)]
    ff, tt, Sxx = signal.spectrogram(seg, fs, nperseg=1024, noverlap=768)
    axes[1].pcolormesh(tt, ff, 10 * np.log10(Sxx + 1e-12),
                       shading='auto', cmap='viridis')
    axes[1].set_ylim(0, 4000)
    axes[1].set_xlabel('Time (s)')
    axes[1].set_ylabel('Hz')
    axes[1].set_title('Spectrogram (first 40 s)')
    plt.tight_layout()
    plt.savefig(png_path, dpi=100)
    print(f"kaydedildi: {png_path}")


def visualize(blocks, fec, png_path):
    """Isi haritasi: satir=blok, renk=byte degeri. Sabit alanlar dikey serit."""
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib yok — blocks.png atlandi (pip3 install matplotlib)")
        return
    rows, labels = [], []
    for i, (pos, blk, ok) in enumerate(blocks):
        t = chr(blk[0]) if 32 <= blk[0] < 127 else '?'
        rows.append(blk[:512])
        labels.append(f"duz {i} '{t}'")
    for j, (h, payload, nerr) in enumerate(fec):
        if payload is not None:
            rows.append(np.frombuffer(payload, np.uint8))
            labels.append(f"FEC {j} (cozuldu)")
    width = max(len(r) for r in rows)
    arr = np.full((len(rows), width), np.nan)
    for k, r in enumerate(rows):
        arr[k, :len(r)] = r
    fig, ax = plt.subplots(figsize=(15, 1.5 + 0.55 * len(rows)))
    im = ax.imshow(arr, aspect='auto', cmap='viridis', interpolation='nearest')
    ax.set_xlabel('byte pozisyonu')
    ax.set_yticks(range(len(labels)), labels, fontsize=8)
    ax.set_title('QO-100 beacon bloklari (hepsi cozulmus)')
    fig.colorbar(im, label='byte degeri (0-255)')
    plt.tight_layout()
    plt.savefig(png_path, dpi=110)
    print(f"kaydedildi: {png_path}")


def main(path):
    chips, fs, x = wav_to_chips(path)
    per_align = [(a, find_blocks(chips_to_bits(chips, a))) for a in (0, 1)]
    align, blocks = max(per_align, key=lambda t: len(t[1]))
    bits = chips_to_bits(chips, align)
    fec = decode_fec_frames(bits)
    if not blocks and not fec:
        print("hicbir blok bulunamadi — kayit cok kisa ya da sinyal zayif")
        return
    nok = sum(1 for _, _, ok in blocks if ok)
    nfec = sum(1 for _, p, _ in fec if p is not None)
    print(f"\n{len(blocks)} duz blok ({nok} CRC OK), "
          f"{len(fec)} FEC frame ({nfec} cozuldu)")
    total_dur = len(bits) / 400.0
    os.makedirs('data', exist_ok=True)
    # cikti adlari: data/<girdi_adi>_NN_dokum.txt / _NN_block.png
    # NN her kosuda artar, var olan dosyalarin uzerine yazilmaz
    base = os.path.splitext(os.path.basename(path))[0]
    nn = 0
    while any(os.path.exists(os.path.join('data', f'{base}_{nn:02d}_{s}'))
              for s in ('dokum.txt', 'block.png', 'spectrum.png')):
        nn += 1
    txt_path = os.path.join('data', f'{base}_{nn:02d}_dokum.txt')
    png_path = os.path.join('data', f'{base}_{nn:02d}_block.png')
    spec_path = os.path.join('data', f'{base}_{nn:02d}_spectrum.png')
    # dokumu hem ekrana bas hem dosyaya yaz
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        report(blocks, fec, total_dur)
    text = buf.getvalue()
    print(text, end='')
    with open(txt_path, 'w') as fh:
        fh.write(text)
    print(f"kaydedildi: {txt_path}")
    plot_spectrum(fs, x, spec_path)
    visualize(blocks, fec, png_path)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'rec.wav')
