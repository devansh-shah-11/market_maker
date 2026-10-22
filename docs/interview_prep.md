# Interview prep: Assignment 1 (market maker critique)

Everything below was checked against the code in `app/` and `sim/`, and against `design.pdf`.
Where something is my reasoning rather than a measurement, it says so. Say so in the interview too.

---

## 0. How to run the 20 minutes

| Time | What | Goal |
|---|---|---|
| 0-2 | Hotpath walkthrough | Show you understand the whole flow, with the real timestamps |
| 2-5 | Evaluation flaw | Quote the doc, show why it's wrong, give the fix |
| 5-7 | Good decision (flat array) | Say why it's good AND its cost (fixed cap) |
| 7-12 | Exception flaw | Strongest technical point; know code locations |
| 12-17 | Decision-log flaw | Know why you did NOT move the log after the send |
| 17-20 | Their follow-ups | See sections 3 to 6 |

**Pattern for every answer (keeps you structured and honest):**
1. What the design doc says (quote it).
2. What the code actually does (file + function).
3. Why that hurts tick-to-trade.
4. Your fix, and why it doesn't change business logic.
5. The limit of your claim ("I didn't measure this; my argument is about where it sits on the path").

**If you don't know:** say "I haven't verified that, but I'd check by..." It is much better than guessing.

---

## 1. Execution flowchart

### 1a. Simulator loop (the part you do NOT modify: `sim/`)

```
                       SIMULATOR: one thread, simulated clock
                       (simulated time stands still while our handler runs)
 ┌────────────────────────────────────────────────────────────────────────────┐
 │  Time-ordered event queue (ties broken by scheduling order)                │
 │                                                                            │
 │  pop next event                                                            │
 │     │                                                                      │
 │     ├── ArriveAtExchange (scripted participant, or OUR order)              │
 │     │       exchange.Process(...)                                          │
 │     │         ├─ market data msgs ──► schedule DeliverMarketData  (+5 µs)   │
 │     │         └─ reports for us   ──► schedule DeliverExecReport  (+10 µs)  │
 │     │                                                                      │
 │     ├── DeliverMarketData ──┐                                              │
 │     ├── DeliverExecReport ──┼─► CAPTURE TAP: entry = WallClockNow()  ◄─(T0) │
 │     └── Timer ──────────────┘          │                                   │
 │                                        ▼                                   │
 │                        call OnMarketData / OnExecutionReport / OnTimer     │
 │                                                                            │
 │  Inside the app, session.Send(order):                                      │
 │     SendPacket(): sent = WallClockNow()  ◄── FIRST line (T1, tap send)     │
 │                   capture.sends += {delivery_index, sent, packet}          │
 │                   schedule ArriveAtExchange (+10 µs)                       │
 └────────────────────────────────────────────────────────────────────────────┘

   Tick-to-trade (what the assignment cares about)  =  T1 - T0
   What the app's own report measures               =  (handler return) - T0
```

### 1b. `OnMarketData` (hot path)

```
OnMarketData(bytes)
 │
 ├─ header = Decode<Header>(bytes)               (memcpy, not a pointer cast)
 ├─ header.sequence != next_sequence_ ?
 │       yes ─► risk_.LatchSequenceGap()         (standing halt, latched)
 ├─ next_sequence_ = sequence + 1
 │
 ├─ switch(type)
 │     AddOrder / OrderExecuted / OrderCancel / OrderDelete / OrderReplace
 │         └─► book_.Apply(...)
 │               ├─ Lookup(ref)  ──► orders_[ref]       [A] flat array, 1 indexed load
 │               │     (ref >= 1<<20 ? throw out_of_range)
 │               ├─ if order is not ours: update price ladder (one slot per tick)
 │               └─ update cached top-of-book (4 fields); if best level emptied,
 │                  RecomputeBest() scans the ladder toward worse prices
 │
 ├─ Requote()  ─────────────────────────────────────────────► see 1d
 │
 └─ latency_.Record(MarketData, sequence)        one clock read, array write   [E]
        └── this is the end point of the app's OWN latency metric
```

### 1c. `OnExecutionReport` (hot path)

```
OnExecutionReport(bytes)
 │
 ├─ switch(type)
 │     Accepted : orders_.OnAccepted ─► quote.live=true, pending=false
 │                book_.MarkOwn(ref)   (our order is removed from market liquidity)
 │     Replaced : orders_.OnReplaced ─► same;  book_.MarkOwn(ref)
 │     Executed : orders_.OnExecuted ─► update position and cash; live=false if filled
 │     Rejected : orders_.OnRejected ─► pending=false
 │     Canceled : orders_.OnCanceled
 │
 ├─ Requote()                       (state changed, so both sides re-evaluated)
 └─ latency_.Record(ExecutionReport, sequence)
```

### 1d. `Requote` and `QuoteSide` (the core of the hot path)

```
Requote()
 │
 ├─ try {                                                       [X] exceptions
 │     risk_.CheckStanding()    throws SequenceGap / LossLimitBreached if latched
 │     top = book_.top()
 │     RiskGate::CheckBook(top) throws BookInvalid if a side is empty or crossed
 │  } catch (RiskViolation& e) {
 │        if standing error and not halted_ ─► Halt(reason)  ──► see 1f
 │        return;                       (BookInvalid: skip this tick, quotes stay)
 │  }
 │
 ├─ fair_value = CalcMicroprice(top)    integer: (Pb*Qa + Pa*Qb) / (Qa+Qb)
 ├─ credit     = CalcCredit(vol_.Variance(), sampled_mid_)
 │                 sqrt + ceil + max(kMinCredit, ...)   ◄ recomputed EVERY message,
 │                                                        although variance only
 │                                                        changes on the timer
 ├─ QuoteSide(Buy,  RoundToTick(fair - credit, Buy))     bid FIRST
 └─ QuoteSide(Sell, RoundToTick(fair + credit, Sell))    ask second


QuoteSide(side, desired)
 │
 ├─ try { RiskGate::CheckPosition(side, position) }       ◄ runs BEFORE the early-out
 │  catch (RiskViolation&) { return; }    side blocked, the OTHER side still quotes   [X]
 │
 ├─ q = orders_.quote(side)
 ├─ q.pending  OR  (q.live AND q.price == desired) ?
 │       yes ─► return                    (nothing to send: the common case)
 │
 └─ orders_.SendQuote(side, desired, 100)  ──────────────────► see 1e
```

### 1e. `SendQuote` (where the order leaves)

```
SendQuote(side, price, shares)
 │
 ├─ token = next_token_++
 ├─ history_.push_back({time, side, price, shares, token})   unbounded vector
 ├─ log_.Record(...)         fprintf to decisions.log, BEFORE the send        [L]
 │        formatted text (price split into dollars/fraction), stdio-buffered
 │
 ├─ q.live ?  yes ─► build ReplaceOrder
 │            no  ─► build EnterOrder (Day)
 ├─ session_.Send(msg)  ──► SendPacket(): T1 = WallClockNow() recorded by the tap
 └─ q.pending = true; q.pending_token = token      (one request in flight per side)
```

### 1f. Cold paths

```
OnTimer(now)                                (interval set by the scenario)
 ├─ top valid ? sampled_mid_ = (bid+ask)/2 ; vol_.Update(mid)   (EWMA, lambda 0.94)
 ├─ sampled_mid_ != 0 ?  pnl = cash + position * mid
 │       pnl < -kLossLimit ?  risk_.LatchLossLimit()
 ├─ risk_.Halted() && !halted_ ?  Halt("loss limit breached")
 ├─ Reconcile(now): check new history entries (tick grid, size, price > 0)
 └─ latency_.Record(Timer, now)

Halt(reason)            [[gnu::noinline]], runs at most once per run
 ├─ halted_ = true
 ├─ decisions_.Note(...)
 └─ orders_.CancelAll()    cancel live + in-flight orders, bid first then ask
```

### 1g. End-to-end timeline of your worked example (`basic.scn`)

```
t = 1,200,000 ns  participant 2 enters bid 100.01 x 100 (exchange side, sim time)
t = 1,205,000 ns  AddOrder reaches the app (+5 µs)        ── tap entry T0
      OnMarketData: decode, seq ok, Book::Apply (array lookup, ladder, top updated)
      Requote: CheckStanding ok, CheckBook ok, fair value, credit
        QuoteSide(Buy):  position ok; live bid price == desired  -> return (no order)
        QuoteSide(Sell): position ok; live ask 100.04 != desired 100.03
           SendQuote -> history, fprintf log, ReplaceOrder (token 2 -> 5) -> tap T1
      latency_.Record                                       ── app's metric ends here
t = 1,225,000 ns  Replaced report arrives (+10 µs out, +10 µs back from the send time)
      OnExecutionReport -> OnReplaced, MarkOwn, Requote (nothing to send)
```
(`basic.expected` shows `1205000 REPLACE token=2 -> token=5 100 @ 100.0300`.
Sim time stands still inside the handler, so the send is stamped 1,205,000.)

---

## 2. Quick facts to have in your head

| Fact | Value | Where |
|---|---|---|
| Market data latency | 5 µs | design.pdf §1.1, `basic.scn` |
| Order entry latency (each way) | 10 µs | same |
| Tick size | $0.01 | design §2.2.2 |
| Quote size | 100 shares | `config.h` |
| Position limit | 500 shares | `config.h` |
| Loss limit | $100 | `config.h` |
| Order table | `1 << 20` entries (1,048,576) of 16 B = 16 MiB | `config.h`, `Book::Book()` |
| Price ladder | one slot per tick up to $2,000 | `kMaxPrice` |
| Tap timestamp cost | about 20 ns each | comment in `capture.h` |
| `make verify` checks | the stream of orders sent, not `decisions.log` | README |
| Makefile flags | `-O2 -g -Wall -Wextra -Wpedantic`, **no `-Werror`** | `Makefile` |

---

## 3. Questions and answers by section

### A. Hotpath

**Q1. Walk me through one execution of the hotpath.**
Use section 1g. Say: message arrives 5 µs after the exchange publishes it and the tap stamps entry; `OnMarketData` decodes and checks the sequence; the book updates through a flat array and the cached top of book; `Requote` checks the book is valid, computes the microprice and credit; `QuoteSide` runs per side, bid first; the bid is unchanged so no order, the ask changes so `SendQuote` records history, writes the decision log, sends a replace; the tap stamps the send; then the handler logs its completion time and returns.

**Q2. What is "tick-to-trade"?**
The time from a message reaching the application (tap entry) to the order leaving it (tap send). It is a per-order quantity, so it only exists for messages that cause an order.

**Q3. Which handlers are the hotpath?**
`OnMarketData` and `OnExecutionReport`; the header comment in `market_maker.h` says so. `OnTimer` is the cold path (volatility, loss limit, reconcile) and it never sends quotes.

**Q4. Why does an execution report trigger `Requote`?**
Because state changed: an Accepted or Replaced clears `pending`, a fill changes position or empties a side. The strategy says to re-evaluate after every market-data message and every response. A fully filled side must be re-entered while handling that fill.

**Q5. Why did only the ask change in your example?**
After the new best bid, fair value moved; the rounded bid came out at the same tick as the resting bid, the rounded ask came out one tick lower than the resting ask. (I know this from `basic.expected`; I would not claim more precision than that.)

**Q6. Why does the bid side send nothing?**
`QuoteSide` finds the live bid price equal to the desired price, or a request pending, and returns early.

**Q7. What happens on a sequence gap?**
`OnMarketData` latches it before applying the message. The next `Requote` calls `CheckStanding`, which throws `SequenceGap`; the catch sees a standing error and calls `Halt`, which cancels all orders and stops quoting for the rest of the run.

**Q8. Simulated time vs wall time?**
Simulated time decides what happens and when (it is identical every run) and stands still inside a handler. Wall time is what measures performance. The doc itself says never to evaluate performance with simulated time.

---

### B. Evaluation flaw

**Q9. What does the app's latency report actually compute?**
`PrintLatencyReport` (`app/evaluation/analysis.h`) joins each capture *delivery* (market data, report, **and timer**) with the handler's completion record by key, and averages `completed - entry` over all of them. So it is: arrival to handler return, averaged over every message.

**Q10. Why is that not tick-to-trade?**
Three reasons:
1. Messages that send no order are included. Most are cheap no-ops (the early-out in `QuoteSide`), plus timer ticks and reports. They dilute the average.
2. The end point is handler return, not the send. Work after the send counts (the other side, `latency_.Record`).
3. A mean hides the tail; for a trading system the rare slow cases matter.

**Q11. The design doc says non-trading ticks "still do work", so including them is deliberate. Respond.**
That is true and it matters for throughput or queueing, but it is a different metric. Tick-to-trade is defined per order: arrival to send. A slow non-trading message that delays the next one still shows up in the next order's tick-to-trade, because that order's entry timestamp is when it was delivered. So nothing is lost by measuring per order.

**Q12. What exactly is your fix, concretely?**
For every record in `capture.sends`, take `send.wall - deliveries[send.delivery_index].entry`. The tap already records the triggering delivery (that is the `delivery_index` that `main.cpp` uses to write `orders.txt`). Then report median, p99 and max rather than only the mean. It needs no clock reads in the handlers.

**Q13. One message can send two orders (bid then ask). How do you count?**
Per order is fine: the second order naturally has a larger latency because it follows the first. Alternatively report the first order per message. State which you pick. Per order is the honest one.

**Q14. Why median, p99, max?**
Median is the typical case, p99 and max show jitter. The mean can look fine while rare slow cases (for example a buffered-log flush or an exception) are hidden.

**Q15. Doesn't the tap add overhead?**
Yes, about 20 ns per timestamp (`capture.h` says it is not passive). It is outside our code and constant, so it affects before and after runs equally. The design relies on it for exactly that reason.

**Q16. Is p99 meaningful on these tiny scenarios?**
Not on one run: with few orders p99 is nearly the max. That is why `tools/latency_compare.py` pools the orders from many runs of a fixture and keeps slow runs in the tail.

**Q17. What did you see when you compared the two methods?**
*Fill this in from your own run.* Run `tools/latency_compare.py` before the interview (section 6) so you can state the gap and which scenario you used.

---

### C. Good decision: flat array indexed by order ref

**Q18. Why is it good?**
The feed identifies an order only by its ref on nearly every message, so lookup is on the hotpath constantly. Indexing a flat array by ref is one indexed load: no hashing, no collisions, no pointer-chasing. `std::unordered_map` has to hash, find the bucket, and follow a pointer to a separately allocated node, which is often another cache miss.

**Q19. Why can you use refs directly?**
The exchange issues refs in increasing order (design §2.2.1), so the table is dense.

**Q20. Is a 16 MiB array cache-friendly?**
Not entirely: it does not fit in L1 or L2. My claim is narrower: a lookup is a single indexed load, so it costs at most one cache miss, versus several dependent loads for a hash map. `Order` is 16 bytes, so one entry sits within one cache line.

**Q21. What is the cost?**
Fixed capacity: `kMaxOrderRefs = 1 << 20`. Beyond that `Lookup` throws `std::out_of_range`. `Requote` only catches `RiskViolation`, so it propagates up to `main`, which prints an error and exits. For this simulation that is acceptable; in production I'd size it for the day or handle overflow.

**Q22. "Zeroed at startup": why does it matter?**
`orders_(kMaxOrderRefs)` is a `std::vector` constructed with a count, so every element is value-initialised, which touches all pages at startup. So the page-fault cost is paid before the run, not on the hotpath.

**Q23. Alternatives and why not?**
`unordered_map` (hash plus node chasing), `std::map` (O(log n) with pointer-chasing), a sorted vector (insert cost). The flat array wins because the key is a dense increasing integer.

**Q24. Other good decisions in the doc?** (Shows breadth; don't volunteer more than one or two.)
Price ladder (one slot per tick, no tree, no allocation), cached top of book (copy four fields instead of searching), `memcpy` decode (no misaligned access or UB, compiles to a few loads), integer fixed-point prices (exact and deterministic, which `make verify` relies on).

---

### D. Flaw 1: exceptions for risk checks

**Q25. What does the design doc claim?**
§2.2.3: "Exceptions are free when not thrown, and once they are thrown, we are no longer in the hotpath, so latency is no longer critical."

**Q26. Why is that wrong?**
The second half is false in two ways:
- `QuoteSide` runs `CheckPosition` inside a try/catch and, when it throws, just returns; `Requote` then quotes the other side. Bid is quoted first, so if the **bid** side is blocked (long position), the throw happens right before the ask can be sent: it is on the tick-to-trade path. (If the ask is blocked, the bid has already been sent, so the throw is after it. I limited my claim to the bid side for that reason.)
- `CheckBook` runs on every message and throws `BookInvalid` whenever a side is empty or the book is crossed, so it throws repeatedly while that condition lasts (see `empty_side.scn`).

**Q27. What is the "check before early-out" point?**
In `QuoteSide`, `CheckPosition` runs before `if (q.pending || (q.live && q.price == desired)) return;`. So while the position is within one quote size of the limit, a blocked side throws and catches on **every** message, even when no order would have been sent.

**Q28. How near is "near the limit"?**
A buy throws when `position + 100 > 500`, so when position > 400. A sell throws when `position - 100 < -500`. So within one quote size (100 shares) of the limit.

**Q29. How expensive is a throw?**
I did not measure it in this repo. It is general knowledge that throwing involves allocating the exception object and stack unwinding through lookup tables, which is far slower than a branch (on the order of microseconds, versus a handler that is normally much shorter). My argument does not depend on the exact number; it depends on where the throw sits and how often it happens.

**Q30. Your fix?**
Have the checks return a status instead of throwing. The caller branches on it; that is a predictable compare, so minimal cost. Mark the return value `[[nodiscard]]`.

**Q31. The doc's safety argument: you can't ignore an exception but you can ignore a return code.**
Fair concern. `[[nodiscard]]` makes ignoring the result a compiler warning; the Makefile has `-Wall -Wextra` but no `-Werror`, so to make it a hard error I'd add `-Werror=unused-result`. There are only three check functions, so the surface is small.

**Q32. Does your fix change the strategy or risk handling?**
No. Same conditions, same reactions (skip tick, block a side, halt). Only the signalling mechanism changes. `make verify` checks that the orders are identical.

**Q33. The halt logic uses the exception's error code. How does the fix handle that?**
The status carries the same enum (`BookInvalid`, `SequenceGap`, `PositionLimit`, `LossLimitBreached`), and `IsStanding` is applied to it exactly as before.

**Q34. Could you just reorder the checks?**
Moving the unchanged-quote early-out before the position check avoids throws on no-op ticks without changing behavior (a blocked side returns either way). But it would still throw whenever an order would be sent, so the status return is the cleaner fix.

**Q35. Is there any place where exceptions are fine?**
`Halt` runs at most once per run, and `DecisionLog`'s constructor error is startup-time. Exceptions are fine for genuinely rare events. The problem here is that two of the four conditions are per-message conditions.

---

### E. Flaw 2: decision log

**Q36. What does the design doc claim?**
§2.2.4: every order is written to `decisions.log` before it is sent, so if the process dies mid-send the log shows what we intended, and logging after would leave a window where an order is live with no record.

**Q37. What's wrong with it?**
Two things:
1. Cost: `fprintf` formats text on the tick-to-trade path. It happens before `Send`, and the tap stamps the send as the first line of `SendPacket`, so all of it counts. Also stdio is buffered, and when the buffer fills a `write` syscall happens inside some unlucky handler (a latency spike).
2. The guarantee is not real: `DecisionLog` uses `fopen` + `fprintf` with no `fflush`/`setvbuf`, so entries sit in the process's buffer. If the process crashes, the most recent entries are lost, so it isn't a true write-ahead log.

**Q38. Why not just move the logging after the send?**
That would take it off the path but it reopens exactly the window the doc is worried about (order live, no record). It would make the design worse on its own stated goal, so I kept the write before the send and made it cheap.

**Q39. Your fix?**
A fixed-size binary record copied with `memcpy` into a preallocated memory-mapped file, decoded into text after the run. No formatting, no buffer-flush syscall, and because the kernel owns the mapped pages the entries survive a process crash.

**Q40. What does the mmap fix not protect against?**
An OS crash or power loss, unless pages are `msync`ed, which costs more. The doc's scenario is "if the process dies mid-send", which the mmap covers.

**Q41. Page faults on a mapped file?**
First touch of a file-backed page can fault. I'd preallocate the file to its full size and touch the pages at startup so no fault happens on the hotpath.

**Q42. Why not a background thread writing the log?**
The record would still have to get to the other thread, and anything in a plain in-memory queue is lost on a crash. It also adds complexity the simulator (single-threaded, and `Send` is thread-checked) doesn't call for. mmap is simpler and gives the guarantee.

**Q43. Why not `fflush` after each record?**
That is a `write` syscall per order, on the path, which is worse for latency. It trades the guarantee for a bigger cost.

**Q44. Does the fix change behavior?**
No. Orders are identical (`make verify` diffs only the order stream). The log's on-disk format changes, so I'd add a small decoder.

**Q45. What if the mapped file fills up?**
Size it for the expected order count of a run; if it must be unbounded, rotate files outside the hot path. (For this assignment: it is a sizing decision, same as the order table's cap.)

---

## 4. Cross-cutting and curveball questions

**Q46. Which of your two flaws matters more?**
My reasoning (not measured): the log runs on **every order sent**, so it affects steady-state tick-to-trade; exceptions only occur in specific conditions (book invalid, position near limit) but cost far more each time they occur. So I'd call the log the bigger steady-state cost and the exceptions the bigger tail cost. How much each matters depends on how often those conditions occur in real traffic.

**Q47. How would you prove your fixes help?**
Per-order tick-to-trade from the tap (my evaluation fix), before and after, on the same scenarios, many runs pooled, comparing median and p99; plus `make verify` to show behavior is unchanged. For the exception flaw use `position.scn` and `empty_side.scn`, since they trigger those paths.

**Q48. Why these four decisions?**
The assignment limits the critique to decisions discussed in the design doc. These are the ones with the clearest, code-verifiable effect on tick-to-trade.

**Q49. What other flaws in the doc did you leave out?** (Know these, because the interviewer may ask.)
- **Credit recomputed every message** (`CalcCredit`: sqrt, ceil, double math) although variance only changes on the timer. Fix: compute it on the timer and cache; output is identical.
- **`history_` is an unbounded `std::vector`** appended to in `SendQuote`: growth reallocates and copies on the hotpath. Fix: reserve up front.
- **No warm-up**: the doc says everything fits in cache, but after an idle gap the caches and branch predictors can be cold, so the first message after a quiet period can be slower.
- (Minor) `Halt` being `[[gnu::noinline]]` is a reasonable choice since it runs at most once.

**Q50. Does your critique conflict with "don't change the strategy / risk handling"?**
No: none of my fixes change a price, a size, a risk condition, or its reaction. They change how the checks signal, how the log is written, and how latency is measured.

**Q51. How does the evaluation flaw connect to the other two?**
The mean hides the rare slow cases, and a log-buffer flush or an exception would be exactly such cases. Per-order median/p99/max would expose them.

**Q52. "Can the tap tell you which message caused an order?"**
Yes: `SendRecord` stores the `delivery_index` of the delivery in progress when `Send` was called. Orders can only be sent from inside a callback, so the attribution is exact (`capture.h`).

**Q53. Why is `Send` stamped first thing in `SendPacket`?**
So nothing in the simulator's own work after that point counts toward tick-to-trade. It also means everything our app does before calling `Send` (including the `fprintf`) does count.

**Q54. What is the biggest limitation of your critique?**
I argued from code and the design, and did not benchmark the individual fixes. I can say where the costs sit and how often they occur, but not exactly how many nanoseconds each saves.

---

## 5. Where each claim lives in the code (be able to open these)

| Claim | File and function |
|---|---|
| Handlers, `Requote`, `QuoteSide` | `app/market_maker.cpp` |
| Position check before the early-out | `QuoteSide`: `CheckPosition` in a try/catch, then `if (q.pending \|\| ...) return;` |
| Checks that throw | `app/risk/risk.h` (`CheckStanding`, `CheckBook`, `CheckPosition`) |
| Decision log (`fprintf`, no flush) | `app/execution/decision_log.h` |
| Log before send; history append | `OrderManager::SendQuote` in `app/execution/orders.cpp` |
| App's latency metric | `app/evaluation/analysis.h`, `latency_log.h` |
| Tap fields (entry, send, delivery_index) | `sim/runtime/capture.h`, `SendPacket` in `sim/runtime/simulator.cpp` |
| Flat array, cap, lookup | `Book::Book`, `Book::Lookup` in `app/market_data/book.cpp`; `kMaxOrderRefs` in `app/config.h` |
| Credit recomputed per message | `Requote` calling `CalcCredit`, `app/strategy/pricing.h` |
| Doc claims | `design.pdf` §2.1 (evaluation), §2.2.1 (array), §2.2.3 (exceptions), §2.2.4 (log) |

---

## 6. Do these before the interview

1. Build and verify:
   ```sh
   make && make verify
   ```
2. Run the comparison script and note the gap (for Q17):
   ```sh
   python3 tools/latency_compare.py --runs 20
   ```
   Write down: scenario(s), the app's mean, and the tap's per-order median/p99/max.
3. Trigger the exception paths and look at the decision log:
   ```sh
   ./build/market_maker sim/fixtures/position.scn --out /tmp/pos
   ./build/market_maker sim/fixtures/empty_side.scn --out /tmp/empty
   ```
4. Be able to open and read `QuoteSide`, `risk.h`, `decision_log.h`, `SendQuote`, `analysis.h` and `capture.h` without help.
5. Re-read your own critique once and be ready to defend every sentence. If a sentence says more than you can justify, soften it.
6. Fix the AI/collaboration statement so it honestly covers how you used Claude on the critique itself, since the interview will not allow outside help and they may ask.

## 7. Possible questions: `memcpy` decode, sequence gap, `Halt`

### 7a. `memcpy` decode

**Q. Where exactly is the `memcpy` in `OnMarketData`?**
`OnMarketData` does not call `memcpy` itself. It calls `sim::Decode<T>(bytes)` (`sim/messages/wire.h:41-47`), which checks `bytes.size() == sizeof(Msg)` and then does `std::memcpy(&msg, bytes.data(), sizeof(Msg))`. `OnMarketData` calls `Decode` twice per message: once for `md::Header` (line 14, to read `sequence` and `type`) and once inside the `switch` for the full message (`AddOrder`, `OrderExecuted`, and so on). So the header is decoded twice, which is a minor cost.

**Q. Why `memcpy` and not a pointer cast?**
The byte buffer may not be aligned for the struct, so a cast can fault or be slow on some CPUs. A cast also breaks strict aliasing, which is undefined behavior. `memcpy` is always legal. The size is a compile-time constant, so at `-O2` it becomes a few register loads and no function call.

**Q. What makes copying raw bytes into the struct safe?**
The `WireMessage` concept: `std::is_trivially_copyable_v<Msg>` and `sizeof(Msg) <= kMaxPacketSize`. `Encode` does the reverse copy.

**Q. What if the packet is the wrong size?**
`Decode` throws `std::invalid_argument`. Nothing in `OnMarketData` catches it. It unwinds to `main`.

### 7b. Sequence gap and `LatchSequenceGap`

**Q. What is `LatchSequenceGap`?**
Market data messages carry a sequence number that rises by 1. `OnMarketData` compares it to `next_sequence_`. On a mismatch it calls `risk_.LatchSequenceGap()` (`risk.h:43`), which only sets `sequence_gap_ = true`. "Latch" means set once and never cleared. A mismatch means a message was missed, so the book may be wrong and quoting off it is unsafe.

**Q. Why latch it instead of making it a per-tick check?**
A per-tick error such as `BookInvalid` can fix itself on the next message. A gap cannot: the book stays wrong until it is rebuilt from a snapshot, and this design has no resync. So it is a "standing" error, like `LossLimitBreached`.

**Q. What happens after the latch?**
1. `OnMarketData` still applies the message and calls `Requote()`.
2. `Requote` calls `risk_.CheckStanding()`, which throws `RiskViolation(SequenceGap)`.
3. The catch sees `IsStanding(...) && !halted_` and calls `Halt`.
4. `Requote` returns without quoting.

**Q. Does the check work if the very first message is lost?**
Yes. `next_sequence_` starts at 1 (`market_maker.h:55`) and the exchange starts `next_md_sequence_` at 1 (`exchange.h:71`). If the first message seen is 3, then 3 != 1 and the gap latches. Caveat: the hard-coded 1 would not work for a feed joined mid-session. It would need the starting sequence from a snapshot or the first message.

**Q. What about duplicates or out-of-order messages?**
Anything not equal to the expected number counts as a mismatch, so it halts. That is the conservative choice. Also, after a mismatch the code still does `next_sequence_ = header.sequence + 1`, which resyncs the counter, but the latch stays set.

**Q. Are execution reports sequence-checked?**
No. The exchange keeps a per-participant `next_report_sequence_`, but `OnExecutionReport` never checks it. A lost report would go unnoticed. This is a real gap in the design.

**Q. What would you do in production?**
Request a retransmit or a snapshot, rebuild the book, clear the latch and resume. Halting for the rest of the run is safe but blunt.

### 7c. `Halt` and `[[gnu::noinline]]`

**Q. What does `Halt` do?** (`market_maker.cpp:144`)
Three things: sets `halted_ = true`, writes the reason to the decision log, and calls `orders_.CancelAll()`. Cancelling matters because stale quotes would otherwise rest at the exchange, priced off a book we no longer trust.

**Q. Does halting stop the process or the feed?**
No. Halting means "stop quoting". The exchange does not know the app halted and keeps publishing. `Simulator::Run` keeps calling `OnMarketData` until the event queue is empty or the end time passes. A real halted app also keeps its feed connection open to keep its book current and to see its cancels acknowledged.

**Q. When do we halt?**

| Condition | Detected in | Path to `Halt` |
|---|---|---|
| Sequence gap | `OnMarketData` (`market_maker.cpp:16`) | Latch, then the same message's `Requote()` throws and the catch calls `Halt` |
| Loss limit | `OnTimer` (PnL from the sampled mid) | Latch, then `if (risk_.Halted() && !halted_) Halt(...)` at line 124 |

The gap halts on the message that exposes it. The loss limit halts on the next timer tick after PnL crosses the limit, because PnL is only checked on the timer.

**Q. What does `[[gnu::noinline]]` mean?** (`market_maker.h:45`)
It is the C++11 attribute syntax with the `gnu::` vendor namespace (GCC and Clang), equivalent to `__attribute__((noinline))`. It tells the compiler never to paste `Halt`'s body into its callers. Say "gnu", not "gpu".

**Q. Why mark `Halt` that way?**
`Halt` runs at most once per run. Inlined into `Requote`, its log write and `CancelAll` would sit inside the hot function, make it bigger, use more instruction cache, and can change register allocation and layout for the common path. Keeping it out of line keeps the hot path small.

**Q. Is it the same as the `inline` keyword?**
No. `inline` is mostly about linkage (ODR) and only a hint for optimization. `noinline` is a hard prohibition.

**Q. Is it portable?**
No. MSVC uses `__declspec(noinline)`. C++ has no standard `noinline`. Compilers that do not know `gnu::` may ignore it with a warning.

**Q. Is there a better attribute?**
Possibly `[[gnu::cold]]`: optimize for size, put it in a separate section, treat branches to it as unlikely. C++20 also has `[[unlikely]]`. `noinline` is the simpler, more predictable choice.

**Q. Did you measure the benefit?**
Be honest: no isolated benchmark. For a function called once the effect is probably small. It follows the usual low-latency rule of keeping cold code out of the hot path. Do not claim a number. To verify, compile at `-O2` and check for a `call` to `Halt` in the assembly (at `-O0` nothing is inlined anyway).

**Q. A weakness: what happens on every message after a halt?**
`halted_` only stops `Halt` from running twice. `Requote` starts with `CheckStanding()`, and the latch never clears, so it throws on every later message. The catch skips `Halt` (because `halted_` is true) and returns. A throw/catch costs on the order of microseconds, far more than any inlining effect.
- Message 6 is not concurrent with message 5. The simulator is single-threaded and message 5 is fully done, `Halt` included, before message 6 starts. The issue is that message 6 is a fresh `Requote` call that throws before it ever looks at `halted_`.
- Fix: `if (halted_) return;` at the top of `Requote`. It also keeps the latency stats honest, because `latency_.Record(...)` runs unconditionally after `Requote()` and would otherwise record those slow post-halt samples.
- Be honest that this is low priority: nothing is quoted after a halt, so it costs no trading opportunity.

### 7d. Processing time and the simulator

**Q. What if a message takes a long time to process? What about the next ones?**
In the simulator, simulated time does not advance while a callback runs (`simulator.h` header). `Run` is single-threaded: pop one event, set `now_`, run the callback to completion, pop the next. Later messages simply wait; none are dropped or delayed in simulated time. What is measured is real wall-clock time: `Deliver` records `WallClockNow()` at entry and the tap stamps the send time, so a slow handler shows up as a large latency number.

**Q. And in a real system?**
Messages queue in the socket or NIC buffer, so you react to stale data and quote off an old book. If the buffer overflows you drop packets, which creates exactly the sequence gap that `LatchSequenceGap` catches. That is why the hot path is kept tight.

### 7e. Restarting after a halt

**Q. How would you restart after a halt? Is `halted_ = false` enough?**
No. For a sequence gap, `sequence_gap_` is still latched, so the next `Requote()` makes `CheckStanding()` throw again. Because `halted_` is now false, `Halt` runs a second time and sends another `CancelAll`. The app halts again on the next message.

A resume needs to:
- Add a way to clear the latch in `RiskGate` (`risk.h` has `LatchSequenceGap` and `LatchLossLimit` but no clear), e.g. `ClearSequenceGap()`.
- Rebuild the book from a snapshot, since the gap means the book is wrong. Resuming without this quotes off bad data. (Check whether `Book` has a reset before the interview.)
- Reset `next_sequence_` to the snapshot's sequence plus 1.
- Then set `halted_ = false`. All of this would sit in a new `MarketMaker::Resume()`.

Nothing in the current design triggers a resume. The `Application` interface has only `OnMarketData`, `OnExecutionReport` and `OnTimer`, and the feed has no snapshot message. A real design needs a snapshot or retransmit message, or an operator command.

**Q. Should the loss limit also auto-resume?**
No. It should need a human sign-off, because the strategy was losing money. A gap is a data problem and can be fixed by rebuilding the book. A loss breach is a strategy problem.

**Q. After a sequence gap, can we never trade again that day?**
In this code, yes: the latch is never cleared, so it is halted for the rest of the run. In production, no. Exchanges provide retransmission or snapshot recovery, and firms typically halt, rebuild the book and resume, sometimes after human approval. Say that this halt-forever design is the safe but blunt simplification and describe the resume steps above.
