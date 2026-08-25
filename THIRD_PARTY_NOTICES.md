# Third-party notices

fecraw includes and adapts ideas and implementation structure from the following projects in addition to the dependencies already retained under `ref/`.

## Queqiao

- Project: https://github.com/bojieli/queqiao
- Copyright (c) 2026 Bojie Li
- License: MIT

The protocol-v2 loss estimator, erasure-floor separation, residual-loss FEC sizing, erasure-aware pacing design, and the sliding-window GF(256) RLNC implementation in the `feat/erasure-aware-fec-v2` work were derived from or informed by Queqiao's `internal/lossmodel`, `internal/fec`, and erasure-aware congestion-control design.

The RLNC codec specifically follows Queqiao protocol-1's bounded repair-window structure, GF(256) primitive polynomial `0x11d`, and deterministic RID/index coefficient-generation rule, adapted here to fecraw's packet-oriented TUN data plane and C++ runtime.

The MIT license permits use, modification, and redistribution provided the copyright and permission notice are retained. The repository root `LICENSE` and this notice retain the required attribution for the adapted work.
