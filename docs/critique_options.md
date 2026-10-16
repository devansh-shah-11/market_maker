# Assignment 1 — critique study guide (options + evidence)

Purpose: reference for writing the 1-page critique **myself** (AI policy) and for the 15-min interview.
Numbers marked "measured" come from a scratch copy on macOS (see section 7). Numbers marked "typical" are general knowledge, not measured.

## 0. What the assignment asks

- Role: performance engineer at a market maker. Goal: minimize **tick-to-trade latency** (market data tick in -> order out) without changing strategy behavior. `make verify` diffs `orders.txt` against `sim/fixtures/*.expected`, so orders must stay identical. `sim/` must not be changed.
- Deliverable: 1-page critique in `docs/critique.tex` + a separate page with a collaboration/AI-use statement (not counted in the page limit). Then a 15-min interview, no outside help. Code submission is not required, but I may modify `app/` (incl. `evaluation/`) to collect metrics.
- Scope: only design decisions **discussed in design.pdf**.
- Five prompts in the template:
  1. One possible execution of the hotpath
  2. A bad decision in the evaluation + fix
  3. A good decision in the market maker
  4. Bad decision #1 in the market maker + fix
  5. Bad decision #2 in the market maker + fix
- Paragraph recipe: claim -> evidence (doc section / file:line) -> mechanism -> fix -> trade-off. ~6-8 lines each.

## 1. Q1 — one possible execution of the hotpath

Options (pick one; mention B in one sentence if room):

**A. Tick that moves top of book -> requote both sides (recommended; touches every component).** E.g. another participant adds a bid better than the best:
1. Simulator calls `OnMarketData` (`app/market_maker.cpp:14`); the capture tap stamps entry wall time.
2. Header decoded via `memcpy`; sequence number checked vs `next_sequence_` (mismatch latches a gap).
3. Switch -> `AddOrder` -> `Book::Apply`: flat array lookup by ref, ladder update; order not ours -> `AddLiquidity` improves cached `top_`.
4. `Requote()`: `CheckStanding`, copy 4-field top, `CheckBook`, microprice (integer divide), credit (`sqrt`), round to tick per side, `QuoteSide(Buy)` then `QuoteSide(Sell)`.
5. Each `QuoteSide`: `CheckPosition`; if not pending and price differs -> `SendQuote`: take token, append `history_`, write `decisions.log` line, build `ReplaceOrder`, `session_.Send` (this is the "trade" of tick-to-trade).
6. `latency_.Record` reads clock, returns.
7. ~10 us simulated later `Replaced` arrives in `OnExecutionReport` -> `OnReplaced`, `book_.MarkOwn(ref)` removes our order from the ladder, `Requote()` again (likely no-op).

**B. Tick that changes nothing (common case).** `sim/fixtures/quiet.scn`: "most messages lead to no order". Steps 1-4, then both `QuoteSide` return early. Good lead-in to the evaluation critique.

**C. A fill report.** `OnExecutionReport` -> `OnExecuted` updates position/cash -> `Requote` re-enters a fully filled side.

## 2. Q2 — bad decision in the evaluation

The doc: report = average of (handler end − tap entry) over **every** message, incl. no-order ticks, reports, timer ticks (`app/evaluation/analysis.h:24-52`).

- **E1. Wrong population (recommended).** Goal is tick-to-trade but the mean is dominated by messages that send nothing; timer ticks aren't on the path at all. Speeding up the order path barely moves it; a send-path regression is nearly invisible. Toy example: nine no-op ticks at 50 ns + one order tick at 1000 ns -> mean 145 ns; halve the order path -> 95 ns.
  Fix: the tap already records each `Send` time and the delivery that caused it (`SendRecord.delivery_index`, `.time`). Compute `send.time − delivery.entry` only for deliveries that caused a send. No app instrumentation needed (drop `LatencyLog` + per-handler clock read). Trade-off: lose "total work" view; report no-order time separately if wanted.
- **E2. Mean hides the tail.** Latency is heavy-tailed; one slow tick (exception, vector realloc, `write` syscall) loses queue position and the mean averages it away. Fix: p50/p99/p99.9/max or histogram. Needs enough samples.
- **E3. Wrong endpoint.** Timestamp is taken after `Requote()` returns, so it includes post-send work (ask side, logging); with two orders per tick the first order's latency is shorter than the handler's. Fix: tap's per-order send time (part of E1's fix).
- **E4. Mixed populations.** Timer ticks (`Reconcile`, `std::log`) averaged with market data. Sub-case of E1.
- **E5. Noise / small samples.** ~60 deliveries per fixture, one cold run; startup effects dominate. Fix: repeat N runs, discard warm-up, median across runs. Weaker fit to "doc only".

Recommendation: E1 with E2 + E3 as the fix.

## 3. Q3 — a good decision in the market maker (pick one)

- **G1. Flat array indexed by order ref, not hash map** (`app/market_data/book.cpp:24`, design 2.2.1). Nearly every feed message carries only a ref. One bounds check + one load; no hash, no pointer chase, no allocation. Refs dense/increasing -> recent orders adjacent in cache. 16 B x 2^20 = 16 MB allocated once. Caveats: depends on dense increasing refs; throws past 1M refs; array > L2, relies on small active working set.
- **G2. Preallocated price ladder instead of tree.** Add/remove size = one indexed update, no allocation for new levels. 200,000 slots x 8 B ≈ 1.6 MB/side. Caveat: next-best linear scan (see B6).
- **G3. Cached top of book (deliberate denormalization).** Strategy reads top every message; read = 4-word copy. Cost moved to updates, which only occur when top changes. Caveat: duplicated state must be kept in sync in every add/remove; scan when best empties.
- **G4. Fixed-point integer prices.** Exact, deterministic -> `make verify` can compare exactly. Caveat: credit/volatility use `double`, so "integers throughout" is not fully true (avoid if also picking B4).
- **G5. `memcpy` decode.** No misalignment/UB, compiles to a few loads. Header decoded twice (minor).
- **G6. `[[gnu::noinline]] Halt`.** Keeps rare code out of quoting functions' i-cache footprint; real but small (`[[gnu::cold]]` even better).

Recommendation: G3 (cleanest tradeoff story) or G1 (most concrete). Know the downsides.

## 4. Q4/Q5 — bad decisions in the market maker (candidates)

- **B1. Exceptions for risk checks** (`app/risk/risk.h`, `app/market_maker.cpp:73-108`; design 2.2.3). Strongest.
  Claim: "free when not thrown, and once thrown we're no longer in the hotpath." First half true, second half false: `BookInvalid` (skip tick, resting quotes stay) and `PositionLimit` (side blocked, other side quotes) don't stop the system; next message is immediately back on the hotpath. They recur: `quiet.scn`/`empty_side.scn` start with an invalid book; `position.scn` blocks a side for many consecutive messages (`QuoteSide` throws each time); after a latched halt every message throws from `CheckStanding`.
  Cost of a throw: allocates exception object (+ `std::runtime_error` string copy), table-driven unwinding; much more for the first throw.
  Fix: `[[nodiscard]]` status/enum (or `std::optional<RiskError>`) and branch (~1 ns predictable). Their "can't be ignored" argument: `[[nodiscard]]` + `-Werror=unused-result`, or funnel all checks through one function whose result is consumed once.
- **B2. Synchronous text decision log before every send** (`app/execution/decision_log.h`, `orders.cpp:13`; design 2.2.4). Strong.
  `fprintf` formats text on the hot path, takes stdio lock, copies to buffer; when full (~every 90 lines of ~45 B) a `write` syscall -> spikes. Stated benefit is NOT delivered: `fprintf` buffers in user space, so a crash loses up to a buffer of lines (only per-order `fflush` fixes that, at a syscall per order).
  Fix options: fixed-size binary record into `mmap(MAP_SHARED)` file (memcpy cost, kernel owns pages -> survives process death, format offline; best match to stated goal); in-memory ring drained by another thread (lost on crash unless shared memory); log after send (cheapest, gives up the guarantee). `make verify` only diffs `orders.txt`; keep log's semantic content anyway.
- **B3. Unbounded quote history** (`orders.h:78`, `orders.cpp:12`, `market_maker.cpp:132-142`; design 2.2.4).
  Every order appends ~40 B to a `std::vector` with no `reserve` -> malloc, full copy, page faults at unpredictable moments. Purpose weak: doc says reconciliation against exchange records, but `Reconcile` only checks tick grid / size 100 (true by construction) and never compares to the exchange; decision log already has the same data. Reconcile runs on the timer thread and delays the next delivery.
  Fix: reserve up front (like `LatencyLog`), or fold into decision log, or drop the tautological check.
- **B4. Credit recomputed every message** (`market_maker.cpp:89`, `pricing.h:57`; design 2.2.2).
  Rationale "always priced off the latest volatility" is vacuous: variance and `sampled_mid_` only change in `OnTimer`. Per message `sqrt`, `ceil`, int<->double conversions recompute the same value.
  Fix: compute `credit_` once in `OnTimer`, store. Output unchanged (before first timer variance 0 -> `kMinCredit`, same as now). Small (tens of cycles) but certain and risk-free; easiest to defend.
- **B5. No cache warm-up** (design 2.2.5).
  Claim "nearly every message already runs book and pricing code" misses that the send path (`SendQuote`, `fprintf`, encode, `Send`) runs only when a quote changes — exactly the latency that matters. Simulator/exchange code on the same core also evicts lines between callbacks.
  Fix: exercise the send path against a stub during idle time (no real send, no token consumed). Caveat: simulator delivers back-to-back with no idle gaps and warm-up after `Record()` can flatter results; be ready to explain.
- **B6. Linear scan in `RecomputeBest`** (`book.cpp:131-148`; design 2.2.1). Assumes levels near top are close; breaks with thin book / sweeps (up to 200,000 slots). Fix: hierarchical occupancy bitmap + find-first-set. Weaker: fixtures show no gap; need custom scenario.
- **B7. Single function branching on side.** Call order fixed, branch predictable, negligible. Don't pick.

Pairing: choose from different doc sections. B1 + B2 = biggest. B1 + B4 = safest/easiest to quantify. B2 and B3 overlap (both logging).

## 5. Collaboration/AI statement

Must be honest and factual: who I worked with, which AI tools, how used (e.g. read PDFs, explained code, brainstormed options, ran measurements; final critique written myself). Write it myself.

## 6. Interview prep

Expect "why is that actually slower?" and "what does your fix cost?". Know the downsides listed above.

## 7. Measured baseline (scratch copy, macOS, `-O2`, `make verify` all PASS)

Per-fixture, from `capture.csv` + `latency_log.csv` (ns). Note macOS clock granularity is ~41 ns (24 MHz counter), so 0/41/83 are quantization.

| fixture | all msgs: n / mean / median / max | order-causing msgs (tick-to-first-send): n / mean / median | no-order msgs: mean / median / max |
|---|---|---|---|
| basic | 54 / 878 / 42 / 33625 | 11 / 655 / 250 | 1150 / 42 / 33625 |
| quiet | 48 / 977 / 42 / 35583 | 1 / 3958 | 1520 / 41 / 35583 |
| position | 117 / 594 / 42 / 27750 | 19 / 645 / 209 | 768 / 42 / 27750 |
| sweep | 59 / 716 / 42 / 28459 | 6 / 840 / 312 | 1035 / 42 / 28459 |
| gap | 36 / 1562 / 42 / 26958 | 2 / 3770 | 3321 / 2708 / 26958 |
| loss | 118 / 918 / 42 / 26417 | 15 / 828 / 209 | 1459 / 42 / 26417 |
| empty_side | 42 / 1113 / 42 / 25125 | 3 / 1208 / 375 | 2154 / 167 / 25125 |

Findings:
- **E1/E2 confirmed:** median is ~42 ns (clock quantum) but mean is ~600-1500 ns: the reported mean is driven by a few outliers, not typical behavior. No-order messages have a *higher* mean than order-causing ones, so the reported average is not a tick-to-trade measure.
- **B1 evidence:** in `quiet`, `position`, `gap` the first messages (book invalid -> throw, skipped tick) cost ~2.0-5.5 us each versus ~0-42 ns for steady-state messages (~50-100x). The very first message costs 27-36 us (first throw ever + cold start; not purely the throw). Not yet isolated by counting `__cxa_throw` calls — still to do.
- **B2/B5 hint:** first message that actually sends (e.g. `quiet` seq 4) ~5 us: cold send path + first `fprintf`.
- Caveats: tiny runs (36-118 messages), single run, macOS (exception/syscall costs differ on Linux); re-measure on CIMS.

Scratch copy lives at `/private/tmp/claude-501/-Users-devansh-Desktop-NYU-Sem-3-Technologies-for-Finance-assignment01/0667f051-1512-4666-93b7-75d316a918ee/scratchpad/work`; analysis script `.../scratchpad/an.py`.

## 8. Running on CIMS (Linux)

Copy the project (excluding `build/`), e.g. `rsync -av --exclude build --exclude .claude --exclude DIR "<project dir>/" <netid>@<cims-host>:assignment01/`, then `make && make verify`. Needs g++ 11+ (C++20); on Linux `perf stat/record` is available.

## 9. Next steps

- Count throws (`__cxa_throw`) per fixture; prototype fixes B1/B2/B4 in the scratch copy and re-run `make verify` + the latency breakdown; build a longer scenario for stable stats; user picks one option per question.
