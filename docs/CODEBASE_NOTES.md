# Codebase notes (read this first in a new session)

Goal: skip re-reading the code. Companion file: `critique_options.md` (critique candidates + measured numbers).
**Not yet read in detail:** `sim/exchange/*` (matching), `sim/messages/*` (wire formats), `sim/replay/scenario.cpp`, `sim/runtime/capture.cpp`, `tests/sim_tests.cpp`. Nothing in these notes comes from running code in this session.

## 1. The assignment in one paragraph

Role: performance engineer at a market maker. Goal: minimize **tick-to-trade latency** (market data message arrives -> our order leaves) **without changing trading behavior**. `make verify` diffs the orders sent against `sim/fixtures/*.expected`. Do not modify `sim/`. `app/` (including `app/evaluation/`) may be changed to collect metrics, but code submission is optional.
Deliverable: 1-page critique in `docs/critique.tex` (-> PDF) + a separate collaboration/AI-use statement (not counted in the page limit; write it honestly myself). Then a 15-minute interview with no outside help. Scope: only design decisions **discussed in `docs/design.pdf`**.
Critique prompts: (1) one hotpath execution, (2) one bad decision in the *evaluation* + fix, (3) one good decision in the market maker, (4)+(5) two bad decisions in the market maker + fixes.

## 2. The simulator (`sim/`, fixed environment)

- Single process, single thread, discrete-event. `Simulator::Run` (`sim/runtime/simulator.h:160`) pops the earliest event from a priority queue ordered by (time, scheduling-counter), sets `now_` to its time, handles it. Same-time events run in scheduling order -> fully deterministic.
- Event kinds: `ArriveAtExchange`, `DeliverMarketData`, `DeliverExecutionReport`, `Timer`.
- **Two clocks.** *Simulated time* (`now_`, ns) = the scripted world; it **freezes while an app callback runs**, so code speed can never change which orders are sent or their simulated timestamps. *Wall-clock time* (`sim::WallClockNow()`) is the only thing that measures performance.
- Latencies (scenario file): market data 5 us, order entry 10 us each way. Scripted participants reach the exchange with 0 latency.
- Example timeline: participant order at t=0 -> feed msg reaches app t=5,000 -> app calls `Send` -> arrives at exchange t=15,000 -> feed msg for *our* order reaches app t=20,000 -> `Accepted` reaches app t=25,000. So our own order appears in the feed **before** we learn its order ref (reason for the `own`/`MarkOwn` logic in the book).
- Timer: `timer N` in the scenario schedules `OnTimer` every N ns up to `end`. All fixtures use 1 ms.
- `dropped_market_data` in a scenario drops feed messages by sequence number -> creates a sequence gap (see `gap.scn`).
- **Capture tap** (`sim/runtime/capture.*`, outside app code): records wall time just before each callback (`DeliveryRecord.entry`) and wall time at each `Send` (`SendRecord`, linked to its causing delivery via `delivery_index`). It does NOT record when callbacks return. `Send` can only be called from the simulator thread (else `std::logic_error`).
- App interface (`sim::Application` concept): `OnMarketData(span<byte>)`, `OnExecutionReport(span<byte>)`, `OnTimer(Timestamp)`. Callbacks get raw bytes; the app decodes (`sim::Decode<T>` = memcpy into a packed struct).
- Protocols: prices are fixed-point ints with 4 decimals ($100.25 = 1002500), tick $0.01 (= 100 units). Feed is ITCH-like, order-by-order, with a sequence number from 1 (`A` add, `E` executed, `X` cancel/reduce, `D` delete, `U` replace). Only `A` and `U` carry price/side, so the consumer must remember orders. Our orders are in the feed unmarked. Order entry is OUCH-like (`O` enter Day/IOC, `U` replace, `X` cancel; our own unique **token**). Reports: `Accepted`, `Replaced`, `Executed`, `Rejected`, `Canceled`, with their own sequence number starting at 1. Order ref is learned one round trip after sending.
- Scenario DSL example (`sim/fixtures/basic.scn`): `latency ...`, `timer ...`, `end ...`, then lines `<time_ns> <participant> enter|cancel|replace ...`. Fixtures: basic, quiet, empty_side, gap, loss, position, sweep (each with `.expected`).

## 3. The strategy (fixed; must be reproduced exactly)

- **Fair value** = microprice of best bid/ask **excluding our own orders**: `(Pbid*Qask + Pask*Qbid)/(Qbid+Qask)`, integer division (floors to $0.0001).
- **Credit** (half-spread) = `max(kMinCredit=100 (=$0.01), ceil(2.0 * sigma * mid))`. sigma comes from an EWMA (lambda 0.94) of squared log returns of the mid, **sampled only on the timer**, not per message. `mid = floor((bid+ask)/2)`. Before 2 samples sigma=0 -> credit = $0.01.
- **Quotes:** one Day order of 100 shares per side. `bid = floor((fv-credit)/tick)*tick`, `ask = ceil((fv+credit)/tick)*tick` (rounded away from fair value).
- **When to requote:** after *every* market data message and *every* execution report (not on timer), bid first then ask. Per side: no resting order -> Enter; resting price != desired -> Replace; else nothing. **At most one request in flight per side** (nothing more sent on that side until its response arrives). Tokens are sequential from 1 for each Enter and Replace.
- **Risk:** book invalid (a side empty or crossed) -> skip this requote, quotes stay. Sequence gap -> cancel all, stop forever (latched). Position limit +-500 (a new quote that could exceed it is not sent on that side; other side continues). Loss limit: on timer, PnL = cash + position*sampled_mid < -$100 (`kLossLimit` = 1,000,000 units) -> cancel all, stop forever.
- Parameters live in `app/config.h` (changing them breaks `make verify`).

## 4. The app, file by file (`app/`)

| File | What it does / why |
|---|---|
| `main.cpp` | Loads scenario, runs sim with capture tap on, writes outputs to `build/out/<name>/` (`capture.csv`, `latency_log.csv`, `orders.txt`, `decisions.log`, optional `trace.txt`), prints the latency report. `orders.txt` = sim time + formatted order, used by `make verify`. |
| `market_maker.{h,cpp}` | The three handlers. `OnMarketData`: decode header, check sequence (`risk_.LatchSequenceGap()` on mismatch), switch on type -> `book_.Apply`, then `Requote()`, then `latency_.Record`. `OnExecutionReport`: decode, route to `OrderManager` (and `book_.MarkOwn(order_ref)` on Accepted/Replaced), `Requote()`, record. `Requote()`: risk checks inside a try/catch (exceptions), microprice, credit **recomputed each message**, `QuoteSide(Buy)`, `QuoteSide(Sell)`. `QuoteSide`: position check (try/catch), skip if pending or price unchanged, else `SendQuote`. `OnTimer`: sample mid, update vol, loss-limit check, `Reconcile`, record. `Halt` is `[[gnu::noinline]]`. |
| `market_data/book.{h,cpp}` | Book rebuilt from the feed. `orders_` = flat `vector<Order>` of 2^20 entries (16 B each, ~16 MB) indexed directly by order ref. Per side a "ladder": `shares_at_tick` vector (200,000 slots = $0-$2000 / tick, 8 B each) + `order_count`. Cached `top_` (best price+size each side). Own orders are tracked but excluded from ladders/top (`own` flag; set by `MarkOwn` or when the feed shows a replace of an own order). `RecomputeBest` linearly scans the ladder from the old best when the best level empties. Throws `out_of_range` on bad refs/prices. |
| `strategy/pricing.h` | `BookTop` (+`Valid()`: both sizes > 0 and bid < ask), `CalcMicroprice`, `VolatilityEstimator` (EWMA, `std::log`), `CalcCredit` (`sqrt`, `ceil`), `RoundToTick`. |
| `risk/risk.h` | `RiskGate`: all checks **throw** `RiskViolation(RiskError)`. `CheckStanding` (latched gap/loss), static `CheckBook`, static `CheckPosition`. `IsStanding(e)` distinguishes latched errors. |
| `execution/orders.{h,cpp}` | `OrderManager`: `Quote{live, token, price, open_shares, pending, pending_token}` for bid and ask. `SendQuote`: next token, append `QuoteRecord` to the `history_` vector, **`log_.Record(...)` (fprintf) BEFORE sending**, then `Send(Replace)` if live else `Send(Enter)`, set pending. `OnAccepted/OnReplaced` make the pending order live. `OnExecuted` updates position/cash/open shares. `OnRejected/OnCanceled` clear state. `CancelAll` cancels live and pending tokens, bid then ask. |
| `execution/decision_log.h` | `DecisionLog`: `fprintf` to a file, buffered stdio. Design claim: write-ahead log so a crash mid-send is recoverable. |
| `evaluation/latency_log.h` | `LatencyLog`: preallocated vector; each handler calls `Record(kind, key)` at its end = one `WallClockNow()` + push_back. Keys: market data -> feed sequence, report -> report sequence, timer -> sim time. |
| `evaluation/analysis.h` | `PrintLatencyReport`: joins capture entry stamps with `LatencyLog` completion stamps via a `std::map` and prints the **mean** over **all** deliveries (md + reports + timers) of (handler end - tap entry). |

## 5. What the code does on one tick (hotpath, "scenario A")

Tap stamps entry -> `OnMarketData` -> decode header, sequence check -> `Book::Apply` (array lookup by ref, ladder update, update cached top) -> `Requote` -> try { `CheckStanding`, copy `top_`, `CheckBook` } -> microprice -> `CalcCredit` (sqrt/ceil) -> `RoundToTick` x2 -> `QuoteSide(Buy)`: `CheckPosition` (try) -> unchanged? return : `SendQuote` (token, `history_.push_back`, `fprintf`, build message, `Send` -> tap stamps send time) -> same for Sell -> `latency_.Record`. Roughly 10 us simulated later the `Replaced`/`Accepted` report arrives and triggers `OnExecutionReport` -> `MarkOwn` -> `Requote` (usually no-op).

## 6. Design-doc claims vs. reality (candidates for the critique; details/evidence in `critique_options.md`)

- Evaluation: mean over *all* messages (incl. no-op ticks, timer ticks) is not tick-to-trade and hides the tail; endpoint is handler end, not the order send time. The tap already has per-order send times (`SendRecord`), so the app-side `LatencyLog` is unnecessary.
- Risk by exceptions: "free when not thrown, and when thrown we're off the hot path" is false for `BookInvalid` and `PositionLimit` (recurring, continue to next tick). Fix: status codes.
- Synchronous `fprintf` decision log before every send (formatting + stdio lock + periodic `write` syscall; also doesn't actually survive a crash because stdio is user-space buffered). Fix: binary record to an mmap'd file / ring buffer, or log after send.
- Unbounded `history_` vector (no `reserve`) and a tautological `Reconcile`.
- Credit recomputed every message though its inputs only change on the timer. Fix: compute once in `OnTimer`.
- "No warm-up" claim misses that the send path runs rarely.
- Good decisions: flat array by ref instead of a hash map; preallocated ladder; cached top of book; fixed-point integers; memcpy decode; noinline `Halt`.
- Measured baseline (macOS, tiny runs): median handler time ~42 ns (clock quantum 41 ns) but mean ~600-1500 ns -> mean driven by a few outliers; first throws cost ~2-5 us each.

## 7. Practical commands

```sh
make            # build into build/
make test       # simulator tests
make verify     # run every fixture, diff orders vs .expected (must stay PASS)
./build/market_maker sim/fixtures/basic.scn [--out DIR] [--trace]
./build/sim_replay sim/fixtures/basic.scn   # message trace of a scenario
```

Repo: https://github.com/devansh-shah-11/market_maker.