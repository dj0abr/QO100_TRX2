# TX1 speech compressor

The purpose is to raise average speech modulation while retaining the existing
SSB peak envelope and digital headroom. The GUI controls one amount from
60 to 140 percent, with **100 percent normal/default**. Config/API retain the
0..140-percent range and zero bypass. A future second TX channel owns separate
state and can use zero speech compression for digital modes.

## Signal path

12-kHz PCM -> bounded jitter FIFO -> adaptive sinc clock correction -> mono
speech compressor -> existing liquid 513-tap analytic USB filter -> existing
liquid integer interpolation to 1.2 MS/s -> digital gain 0.5 -> complex envelope
peak limiter -> liquid frequency shifter -> I16/Q16 Pluto transport.

The gain computer is a small application-specific block. The soft-knee
characteristic and attack/release gain smoothing follow the conventional
approach described in the [MathWorks compressor algorithm documentation](https://www.mathworks.com/help/audio/ref/compressor.html).
Liquid continues to execute SSB filtering, interpolation and frequency shifting;
no new dependency or library version is introduced.

## Speech dynamics at 12 kHz

- Power detector: exponential mean of squared mono samples, 5-ms time constant.
- Threshold: -24 dBFS RMS, 6-dB soft knee.
- Gain-reduction time constants: attack 3 ms, release 150 ms.
- At normalized amount `u` (0..1.4), make-up gain is `18*u` dB. Through 100%,
  the effective ratio remains `1/(1-0.75*u)`, exactly as in the QSO-proven version.
  Above 100%, use `R=4+12*(u-1)` and reduction weight `1-1/R`. This joins with
  matching value and slope at 100%, reaching 8.8:1 at 140%. A direct extension
  of the former weight would invert dynamics above 133.3%, so it is not used.
- The amount slews by 100 percentage points per 240 samples (20 ms), retaining
  detector history. Thus 60..140% takes 16 ms and 0..140% takes 28 ms. At zero,
  mono samples pass unchanged once the slew has settled.
- Reset retains the selected amount and clears signal history; silence produces
  silence. The detector has a floor to avoid denormal arithmetic during pauses.

For weak signals below the knee, amounts 60/100/140 percent yield
10.8/18/25.2 dB gain. Loud speech receives less gain. This is finite make-up
gain rather than an AGC that keeps increasing gain during speech pauses.

## SSB envelope protection

The limiter operates **after all FIR filtering and interpolation**, at the
actual 1.2-MS/s output rate, before the magnitude-preserving TX mixer. It thus
includes analytic-envelope and interpolation overshoots. Its nominal magnitude
ceiling is 0.5 (the existing 6-dB digital reserve). I16 quantization and oscillator
rounding add only numerical tolerance to that bound.

A 6000-sample delay provides 5 ms lookahead. For every future sample `k`, compute
the required gain `g[k] = min(1, 0.5/abs(x[k]))`. For the delayed output sample
`n`, the allowed gain is the minimum of 1 and
`g[k] + (k-n)/6000`, for `n <= k <= n+6000`.
A bounded monotonic queue finds that minimum without scanning the full delay.
The resulting attack ramps toward each future peak and meets its constraint
when it reaches the output. An 80-ms release may reduce gain further but never
exceeds the allowed bound. Both I and Q receive the same gain; there is no
component clipping. The fixed delay is present in speech bypass as well.

The final limiter can introduce gain-modulation sidebands when it acts, as any
time-varying envelope processing does. The slow release limits that modulation;
the unchanged USB filter removes out-of-band components created by the main
speech compressor before interpolation. No RF hard clipping is used.

## Validation

`speech_compressor_dsp` checks quiet/loud gain, the 4:1 and 8.8:1 steady-state ratios,
bypass, silence, smooth-setting convergence, burst/impulse envelope limits,
the complete interpolated/mixed TX chain, sideband rejection and independent
TX instances. `compressor_control_udp` checks config/query/set, fractional
amounts, stale/invalid/session controls and RX/PTT isolation.

`tx_udp` measures bypass versus compressed gain through a local mock driver,
checks bounded I16/Q16 peaks, live setting changes without epoch/PTT reset,
packet gaps and exact silence on absent audio/PTT off. No hardware transmitter
is used in these automated TX tests. The opt-in Pluto hardware test exercises
RX and GUI configuration/meter handshake with PTT off; the station operator
performs the final over-the-air speech listening test.
