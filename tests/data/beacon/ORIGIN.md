# Independent AO-40 test vectors

Copyright 2020 Daniel Estevez <daniel@destevez.net>, GPL-3.0-or-later.
From https://github.com/daniestevez/gr-satellites at
`0ff8b614c3ed57110c3497c287bced9a01bbc238`:

- `ao40_frame.f32`: unchanged `python/components/deframers/qa_ao40_fec_deframer_frame.f32`.
- `post_viterbi.bin`, `payload.bin`: byte arrays extracted without GNU Radio
  from `post_viterbi_reference` / `frame_reference` in
  `python/components/deframers/qa_ao40_fec_deframer.py`.

Full license: [GPL v3](../../../external/qo100-beacon-decoder/COPYING).
