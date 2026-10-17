# Design decisions guide: understand first, pick later

The assignment says to limit the critique to **design decisions discussed in `docs/design.pdf`**. This file goes through every one of them. It is organized by the design doc, not by my opinions.

**How to use it**
1. Read Part 0 (concepts) and Part 1 (the hot path).
2. For each decision in Part 2, read the four questions and the **Concept** note.
3. Write your own verdict in the **Your verdict** line. Then check whether you agree with my **tentative read**.
4. Only after that, choose which decisions go in the critique.

**The four questions asked of every decision**
- **Q1. What does the doc claim?** The decision and its stated reasoning.
- **Q2. Where is it in the code?** File and line.
- **Q3. Is it on the hot path?** Does it run between "tick arrives" and "order leaves"?
- **Q4. Does the claim hold up?** What's right, what breaks it, how sure I am, and how we could check.

**About the evidence**
- "Measured here" = from the `basic` run in `build/out/basic/` in this session: 54 callbacks on macOS, one run.
- "Earlier measurement" = from `docs/critique_options.md`, written by an earlier AI session. I haven't re-checked it.
- "Typical" = general knowledge about computers, not measured on your machine.
- The Mac's clock ticks in steps of about 41 ns (earlier measurement), so any figure under about 100 ns is rough.

---

## Part 0: Concepts you need first

**Latency vs throughput.** Latency is how long one thing takes. Throughput is how many things you finish per second. This assignment is about latency: how fast one order goes out after one tick comes in.

**Tick, tick-to-trade, hot path, cold path.**
- A *tick* is one incoming market-data message (or a message from the exchange about our orders).
- *Tick-to-trade* is the real time between the tick arriving and our order leaving.
- The *hot path* is the code that runs in that window. Code that runs rarely or elsewhere (the timer, the halt, startup) is the *cold path*.

**Wall-clock vs simulated time.** Simulated time is the scenario's script, and it freezes while our code runs. Wall-clock time is real time on your machine. Only wall-clock time measures performance.

**Mean, median, tail.** The *mean* adds everything up and divides, so one huge value drags it far. The *median* is the middle value, and it ignores outliers. The *tail* is the slowest few percent (the 99th percentile, or p99, is the value 99% of messages beat). A market maker cares about the tail: one slow tick can lose queue position.

**CPU cache.** Reading from main memory (DRAM) takes roughly 60–100 ns (typical). The CPU keeps recently used data in small, fast caches: L1 around 1 ns, L2 a few ns, L3 tens of ns (typical). Memory moves in **cache lines** of 64 bytes. A *cache miss* is when the data isn't in cache, and the access is slow.
- *Working set* = the memory you actually touch recently. *Allocated memory* = everything you reserved. A program can allocate 16 MB but touch only a few KB at a time, and the touched part is what matters.
- *Instruction cache* (i-cache) is the same idea for the program's code.

**Branch prediction.** The CPU guesses which way an `if` will go and runs ahead. A correct guess costs nothing. A wrong guess costs around 10–20 cycles (typical). Patterns that repeat (like alternating bid, ask, bid, ask) are predicted well.

**Heap allocation.** `new`, `malloc`, and a growing `std::vector` ask the system for memory. This is slow compared to ordinary instructions and its time varies, so it's avoided on a hot path. `reserve` on a vector allocates once up front.

**System call and page fault.** A *system call* (like `write`) asks the operating system to do something. It costs microseconds (typical) and the time varies. A *page fault* happens the first time memory is touched: the OS has to map a real page.

**stdio buffering.** `fprintf` doesn't write to the file immediately. It copies text into a buffer inside your program (typically around 4 KB), and when it fills, one `write` system call sends it all. Text still in that buffer is lost if the process dies.

**Exceptions.** `throw` jumps out of a function to a matching `catch`. Compilers use the "zero-cost" model: the code that sets up a `try` is free when nothing is thrown. But an actual `throw` is expensive: it allocates an exception object, builds the error message, and *unwinds* the stack by looking up tables frame by frame. Typical cost is microseconds, versus nanoseconds for a normal function call. The first throw in a process is the slowest.

**Fixed-point integers vs floating point.** Fixed-point stores $100.25 as the integer 1002500 (4 implied decimals). Integer math is exact and the same on every machine. Floating point (`double`) is approximate. A 64-bit integer divide takes tens of cycles (typical).

**Reading bytes as a struct (`memcpy` vs cast).** To read a message from raw bytes you can cast the pointer (`(Msg*)bytes`) or copy with `memcpy`. A cast can be misaligned (crashes or is slow on some CPUs) and breaks C++'s aliasing rules (undefined behavior). `memcpy` is always legal, and compilers turn small fixed-size copies into a few register loads.

**Denormalization.** Keeping a duplicate copy of data, in the form you read it most often, so reads are cheap. The price: every update has to keep the copy in sync.

**Hash map vs array.** A hash map finds a key by computing a hash, then often follows a pointer. An array indexed directly by the key is one load. Direct indexing only works if keys are small, dense numbers.

**Inlining, `noinline`, `cold`.** The compiler can paste a small function's body into its caller (*inlining*), which removes the call cost but makes the caller bigger. `[[gnu::noinline]]` forbids that. `[[gnu::cold]]` tells the compiler a function is rarely run, so it can place it away from hot code.

**Write-ahead log.** A log written *before* you act, so that after a crash you can see what you intended. It only works if the log record actually survives the crash.

**Warm-up.** Running code or touching data ahead of time so it's already in cache when it matters.

---

## Part 1: The hot path in this app

The tick-to-trade path is: tap stamps entry → callback → decode → update book → `Requote` → price → `SendQuote` → `Send` (tap stamps the send).

| Step | Code | Runs |
|---|---|---|
| Decode header, sequence check | `app/market_maker.cpp:15-19` | every feed message |
| Decode body, update book | `market_maker.cpp:21-37`, `book.cpp` `Apply` | every feed message |
| Report handling, `MarkOwn` | `market_maker.cpp:43-67` | every execution report |
| Risk checks + read top of book | `market_maker.cpp:75-85` | every message |
| Microprice, credit, rounding | `market_maker.cpp:87-92` | every message when the book is valid |
| Position check, compare to resting | `market_maker.cpp:95-105` | each side, every message |
| **`SendQuote`**: history, log, encode, `Send` | `app/execution/orders.cpp:7-34` | **only when a quote changes** (11 of 54 callbacks in `basic`) |
| `latency_.Record` | `market_maker.cpp:40, 70, 129` | every callback, after any send |
| `OnTimer` | `market_maker.cpp:110-130` | every 1 ms of simulated time |

Two things to keep in mind:
- Most callbacks do everything *except* the send. The send path is rare but is the part the assignment cares about.
- When a callback sends two orders (bid then ask), the second order's latency includes everything before it, including the first order's send work.

---

## Part 2: The 13 decisions

The doc's section for each is in brackets.

### D1. Evaluation: average latency over every message [§2.1]

**Concept.** The app has to measure its own speed. The simulator's capture tap stamps the wall time when each message arrives and when each order leaves. On top of that, the app's `LatencyLog` stamps when each callback *finishes*, and the report averages (finish − arrival) over all messages.

**Q1. The doc claims.** "Not every tick results in a trade ... most market data messages leave our quotes unchanged, and the timer and the exchange's responses do work of their own. To account for all of that work", each handler records a timestamp when it finishes. "Joining the two gives the time each handler took ... and the latency report averages that over every message in the run." The log is cheap: a preallocated array, no I/O, one clock read per handler.

**Q2. In the code.**
- `app/evaluation/latency_log.h:30-37` (`Record`: one clock read + `push_back` into reserved space)
- Call sites: `app/market_maker.cpp:40, 70, 129`
- `app/evaluation/analysis.h:24-52` (the join and the mean)
- The tap's own send records (`SendRecord`) are in `sim/runtime/capture.*`, documented in the README.

**Q3. On the hot path?**
- The report in `analysis.h` runs after the run, so it's off the hot path.
- `Record` runs at the end of every callback, *after* `Requote`, so after any order has already left. It adds one clock read (about 20 ns per the README) before the callback returns, so it doesn't delay an order, but it does add to the callback's measured time.
- The question for this decision is not whether it's slow. It's whether it measures the right thing.

**Q4. Does the claim hold up?**
- *What's right:* the log is cheap and preallocated. If the goal were "total work per message", an average over all messages would be a fair measure.
- *What doesn't match the assignment's goal:* the goal is tick-to-trade, and this metric differs from it in several ways:
  1. **Wrong population.** In `basic`, 43 of 54 callbacks sent nothing (and 10 of the 54 are timer ticks that never send). The average is mostly "nothing to do" messages.
  2. **Wrong endpoint.** It stops at the end of the callback, not when the order leaves. In callbacks that send two orders, the first leaves earlier than the callback ends.
  3. **The mean hides the tail.** Measured here: mean over all 54 callbacks = **2,704 ns**, median = **83 ns**. The first callback alone took **116,083 ns**, about 80% of the total time of all 54.
  4. **It's redundant.** The tap already records each send's wall time and which delivery caused it. Subtracting the delivery's entry time gives tick-to-trade directly, with no `LatencyLog` at all.
- *Measured here:* tick-to-first-send over only the 11 callbacks that sent: mean **1,402 ns**. That's about half the reported 2,704 ns and measures a different thing.
- *Caveats:* one run, 54 messages, macOS clock steps of about 41 ns.
- *How to check:* compute `send.time − delivery.entry` from `capture.csv` for deliveries that caused sends, and compare mean, median and p99 against the existing report, on a longer scenario.
- *Tentative read:* the clearest flaw of the 13, with several separate angles, so you'd choose which angle to argue.

**Your verdict:** ______

---

### D2. Book: order lookup, flat array indexed by ref [§2.2.1]

**Concept.** The feed's `Executed`, `Cancel` and `Delete` messages carry only an order ref, so on almost every message the app must find "which order is this?". A hash map would hash the ref and often follow a pointer. An array indexed by ref is a single load.

**Q1. The doc claims.** Orders are stored "in a flat array indexed directly by ref. The exchange assigns refs in increasing order, so the array is dense, and a lookup is a single indexed load: no hashing, no collisions, and no pointer chasing." A hash map must hash and, for `std::unordered_map`, "follow a pointer into a separately allocated node, which is often a cache miss". The array is "allocated and zeroed once, at startup, for up to about a million refs (16 MB)".

**Q2. In the code.**
- `app/market_data/book.h:64` (`std::vector<Order> orders_`)
- `book.h:45-51` (`Order`: price, shares, side, live, own; 16 bytes, since 16 MB / 2^20 = 16 B)
- `book.cpp:19` (constructor allocates `kMaxOrderRefs` entries)
- `book.cpp:24-29` (`Lookup`: a bounds check, throws `out_of_range` if too large)
- `app/config.h:32` (`kMaxOrderRefs = 1 << 20`)

**Q3. On the hot path?** Yes. Every feed message about an existing order (`E`, `X`, `D`, `U`) calls `Lookup`, `Add` calls it too, and `MarkOwn` on reports.

**Q4. Does the claim hold up?**
- *What's right:* a lookup is one bounds check and one load, with no hashing and no allocation. Consecutive refs sit next to each other (4 orders per 64-byte cache line), and the active orders are recent refs, so they stay cached. The startup zeroing moves page faults to before the run.
- *What it depends on:* the claim that refs are dense and increasing comes from the doc. The trace is consistent (refs 1, 2, 3, ...), but I haven't read the exchange code to confirm.
- *Limits:* a ref of 1,048,576 or more throws `out_of_range`. Nothing catches it in `OnMarketData`, so it would unwind out of `Run` and `main` would exit with an error. And "no cache misses" only holds for recently touched entries: the table is 16 MB, bigger than a typical L2, so an old order's cancel could miss.
- *Tentative read:* a sound decision with a clear trade-off. A good candidate for "good decision".

**Your verdict:** ______

---

### D3. Book: price levels in a ladder [§2.2.1]

**Concept.** To compute the best bid and ask you need the total size at each price. A tree map (`std::map`) keeps prices sorted but allocates a node per level. A *ladder* is an array with one slot per possible price tick.

**Q1. The doc claims.** "The total size resting at each price is kept in a ladder: one preallocated slot per tick, per side, covering prices up to $2,000. Adding or removing size at a price is a single indexed update, with no tree to walk and no allocation when a new price level appears."

**Q2. In the code.**
- `app/market_data/book.cpp:11` (`kLadderSize = kMaxPrice / kTickSize` = 200,000)
- `book.cpp:19-22` (allocation, zero-filled)
- `book.h:53-56` (`Ladder`: `shares_at_tick`, `order_count`)
- `book.cpp:100-115` `AddLiquidity` (line 103 is the indexed update), `book.cpp:117-127` `RemoveLiquidity`
- `app/config.h:29` (`kMaxPrice`)

**Q3. On the hot path?** Yes: every add, reduce, delete or replace of a *market* (non-own) order updates a slot.

**Q4. Does the claim hold up?**
- *What's right:* each update is one indexed add, with no allocation and no pointer chasing. Levels near the best price are adjacent in memory (8 per cache line), so updates are cache-friendly.
- *Costs:* 200,000 slots × 8 B = 1.6 MB per side, 3.2 MB for both, almost all unused. Prices of $2,000 or more, or zero or less, throw `out_of_range` (`book.cpp:32-34`). For one instrument near $100 that doesn't matter.
- *Link to D4:* the ladder is also what makes the "scan to find the next best level" possible (and potentially slow).
- *Tentative read:* sound. Another good candidate.

**Your verdict:** ______

---

### D4. Book: cached top of book [§2.2.1]

**Concept.** *Denormalization.* The strategy reads the best bid and ask on every message. Instead of searching for them, the book keeps a duplicate copy of exactly those four numbers and updates it whenever the best changes.

**Q1. The doc claims.** The book "keeps a small cached copy of the best price and size on each side, updated incrementally whenever an order at or better than the best changes. Reading the top of book is then a copy of four fields." When the best level empties, "the next one is found by scanning the price level ladder from the previous best toward worse prices; levels near the top of the book are close together, so the scan is short."

**Q2. In the code.**
- `app/market_data/book.h:42` (`top()`), `book.h:67` (`top_`); `BookTop` is in `app/strategy/pricing.h:17-24`
- `book.cpp:106-114` (`AddLiquidity` updates the best), `book.cpp:124-126` (`RemoveLiquidity` calls `RecomputeBest` if the removed order was at the best price)
- `book.cpp:131-148` `RecomputeBest` (the scan loop is lines 143-145)
- Read at `market_maker.cpp:77`

**Q3. On the hot path?** Yes: read on every message, updated on every add/remove at the best.

**Q4. Does the claim hold up?**
- *What's right:* reading the top is a 32-byte copy. Updates cost a couple of comparisons. A partial reduce at the best level calls `RecomputeBest`, but the loop exits at once because the level is still non-empty.
- *The assumption:* the scan from the old best to the next non-empty level costs one step per empty tick in between. In `basic` and `sweep` the gap is one tick. But if the best level is removed and the next level is far away (a thin book, or a large sweep), the loop walks up to 200,000 slots (1.6 MB). The doc's "levels near the top are close together" is an assumption about the data, not a guarantee.
- *Other risk:* duplicated state has to stay in sync in every code path, including the own-order handling (`MarkOwn`).
- *How to check:* write a scenario with a wide gap behind the best level and time that callback.
- *Tentative read:* good for the common case, with a known weak spot. It could also be argued as a "bad" decision for an adversarial book, but the fixtures give no evidence.

**Your verdict:** ______

---

### D5. Book: decoding with `memcpy` [§2.2.1]

**Concept.** See "Reading bytes as a struct" in Part 0.

**Q1. The doc claims.** Each handler "copies [the raw bytes] into the matching packed struct with `memcpy` (via `sim::Decode`), rather than casting the byte pointer ... This avoids misaligned access and undefined behavior, and the compiler reduces a small fixed-size copy to a few loads."

**Q2. In the code.**
- `sim/messages/wire.h` `Decode` (a size check, then `std::memcpy`)
- Feed: `app/market_maker.cpp:15` (header), then `23, 26, 29, 32, 35` (full message)
- Reports: `market_maker.cpp:44` (header), then `47, 53, 59, 62, 65`

**Q3. On the hot path?** Yes, once or twice per message. Note the header is decoded first and then the whole message (which includes the header) is decoded again, so the header bytes are copied twice.

**Q4. Does the claim hold up?**
- *What's right:* `memcpy` is the legal and portable way. At `-O2` the compiler reduces a fixed-size copy of a struct of at most 64 bytes to a few loads, so the cost is small.
- *Minor issues:* the second decode repeats the header copy, and `Decode` copies the whole message even when only a few fields are needed. The size check throws only on malformed packets, which the fixtures don't contain. The impact is a few nanoseconds at most.
- *Tentative read:* sound, with low impact. Usable as a "good decision", but you'd need to say honestly that it's a small effect.

**Your verdict:** ______

---

### D6. Pricing: integer price arithmetic [§2.2.2]

**Concept.** See "Fixed-point integers" in Part 0.

**Q1. The doc claims.** "Prices are 64-bit fixed-point integers throughout. The microprice is a single integer division, and rounding to the tick grid is integer arithmetic as well. The result is exact and identical on every machine and compiler, which is what allows `make verify` to compare our output exactly."

**Q2. In the code.**
- `app/strategy/pricing.h:31-36` (`CalcMicroprice`), `pricing.h:64-69` (`RoundToTick`)
- `app/config.h:17` (`kMinCredit` is in price units)

**Q3. On the hot path?** Yes: every `Requote` with a valid book.

**Q4. Does the claim hold up?**
- *What's right:* the microprice is two multiplies, an add and one 64-bit divide (tens of cycles, typical). Results are exact. The products stay well inside 64 bits (prices are under 2×10⁷ and sizes under 2³²).
- *"Throughout" is not quite true:* the credit is computed in `double` with `std::sqrt` and `std::ceil` on every message (see D7), and volatility uses `std::log` on the timer. The determinism claim still mostly holds, but `std::log` isn't required to give identical results on every platform.
- *Tentative read:* good for the microprice, with an overstated claim. The double part in the same file is the real performance question (D7).

**Your verdict:** ______

---

### D7. Pricing: credit recomputed on every message [§2.2.2]

**Concept.** Recomputing a value from inputs that haven't changed is wasted work. The usual fix is to compute it when the inputs change and store the result.

**Q1. The doc claims.** "The credit depends on the volatility estimate and the mid price sampled on the timer. The application recomputes it from those inputs on every message, so that every quote is always priced off the latest volatility estimate."

**Q2. In the code.**
- `app/market_maker.cpp:89` (`CalcCredit(vol_.Variance(), sampled_mid_)`, with the comment "Always price off the latest volatility estimate")
- `app/strategy/pricing.h:57-60` (`sqrt`, multiply, `ceil`, `max`)
- The inputs are written only in `OnTimer`: `market_maker.cpp:113-114` (`sampled_mid_ =`, `vol_.Update`)

**Q3. On the hot path?** Yes: every `Requote` that gets past the risk checks, including the messages that send nothing.

**Q4. Does the claim hold up?**
- *The claim doesn't hold:* "latest" is vacuous. Between timer ticks nothing changes the variance or the sampled mid, so every message recomputes the same number.
- *Cost per message:* convert integer to `double`, square root (a hardware instruction, about 15–20 cycles, typical), multiply, `ceil`, convert back, `max`. Tens of cycles, likely under 20 ns (typical, not measured here), so the effect is small but certain.
- *Fix:* compute the credit once at the end of `OnTimer` (after the volatility update), store it in a member, and have `Requote` read it. It starts at `kMinCredit`, which is what `CalcCredit` returns when variance is 0, so the order stream is identical and `make verify` should still pass.
- *Trade-off:* one more member to keep consistent, but it has a single writer (`OnTimer`). The spec says "we do not requote on timer ticks", so caching changes when nothing goes out.
- *How to check:* a micro-benchmark of `CalcCredit`, or compare tick-to-send over a long run. Under the Mac's 41 ns clock steps the saving may not be visible.
- *Tentative read:* a flawed rationale with a tiny but safe fix. The easiest of the market-maker flaws to defend; not the largest.

**Your verdict:** ______

---

### D8. Pricing: one function for both sides [§2.2.2]

**Concept.** Branch prediction and code duplication. One function with an `if (side == Buy)` is shorter than two near-copies. Making `side` a template parameter would produce two specialized copies with no branch.

**Q1. The doc claims.** "Several calculations differ only slightly between the two sides: bids round down and asks round up, and the position limit is checked in opposite directions. Both sides can thus be handled by a single function that simply branches depending on which side it's given."

**Q2. In the code.**
- `app/market_maker.cpp:95-108` (`QuoteSide`), called at lines 91 and 92
- `app/strategy/pricing.h:64-69` (`RoundToTick`), `app/risk/risk.h:67-73` (`CheckPosition`), `app/execution/orders.h:45` (`quote(side)`)

**Q3. On the hot path?** Yes: both sides are evaluated on every `Requote`.

**Q4. Does the claim hold up?**
- Each branch is cheap, and the pattern is strictly alternating (bid, then ask), which predictors learn easily. The functions are small and may be inlined, in which case the compiler can fold the constant `side`. A template version would save at most a few cycles and duplicate code.
- The doc's argument is about code reuse, not speed. It's a reasonable choice and also a negligible performance effect either way.
- *How to check:* look at the compiled code to see whether `QuoteSide` is inlined.
- *Tentative read:* neutral. A weak candidate for either "good" or "bad".

**Your verdict:** ______

---

### D9. Risk: checks implemented with exceptions [§2.2.3]

**Concept.** See "Exceptions" in Part 0. An error can be reported by *returning a status* (a bool or enum the caller must check) or by *throwing*. Returning is a branch costing about a nanosecond. Throwing costs microseconds.

**Q1. The doc claims.** "Exceptions are free when not thrown, and once they are thrown, we are no longer in the hotpath, so latency is no longer critical. Thus, for every potential risk, we run the check, then throw an exception if we need to abort the quote. Exceptions cannot be accidentally ignored in the same way a return code can. So, this is the safest approach, and one that won't incur a performance penalty on the hotpath."

**Q2. In the code.**
- `app/risk/risk.h:21-78` (`RiskError`, `RiskViolation`, `RiskGate`; the throws are at lines 50, 53, 59, 70)
- `app/market_maker.cpp:73-93` (`Requote`: `try` around `CheckStanding`, `top()`, `CheckBook`; the `catch` at 79-85 returns)
- `market_maker.cpp:95-108` (`QuoteSide`: `try` around `CheckPosition`, the `catch` returns)
- `market_maker.cpp:144-148` (`Halt`)

**Q3. On the hot path?** The check calls run on every message, for both sides. The throws also happen *inside* the tick-to-trade path:
- `BookInvalid` is thrown whenever a side of the book is empty or crossed (`risk.h:59`). In `basic`, the first two feed messages each throw. The comment at `market_maker.cpp:83` says the quotes stay and the next tick continues.
- `PositionLimit` is thrown for the blocked side on every message while the position is at the limit. `QuoteSide` catches it and returns, and the *other* side's `QuoteSide` then runs in the same callback, so its order leaves after an exception was thrown and caught.
- After a halt is latched, `CheckStanding` throws on every later message.

**Q4. Does the claim hold up?**
- *The first half is right:* a `try` block costs nothing when nothing is thrown (zero-cost model, typical).
- *The second half is wrong:* BookInvalid and PositionLimit don't stop the system. They recur, and the next message is back on the hot path. "Once thrown we're no longer in the hotpath" is not true for them.
- *Cost of a throw (typical, not measured here):* allocate the exception object, build a `std::runtime_error` with its message, unwind through the frames using lookup tables. About one to several microseconds each, against tens of nanoseconds for the rest of a callback.
- *Measured here:* in `basic`, the first two feed messages (book invalid, a throw each) took **116,083 ns** and **6,917 ns**, against a median of 83 ns. The first includes cold-start effects, so it's not purely the throw. *Earlier measurement:* skipped ticks in other fixtures took 2.0–5.5 µs against about 0–42 ns for steady-state messages.
- *Not yet isolated:* I haven't counted the throws or timed one.
- *Fix:* have the checks return a status (an enum, or `std::optional<RiskError>`) and branch on it. Make it `[[nodiscard]]` and compile with `-Werror=unused-result` so it can't be silently ignored, which answers the doc's safety argument. Behavior stays identical.
- *Trade-off:* the discipline of checking every status. `Halt` and the latched flags stay as they are.
- *How to check:* count `__cxa_throw` calls per fixture, then time `Requote` on a throwing message with and without the fix.
- *Tentative read:* the strongest flaw in the market maker, with the doc's own reasoning contradicted by its own code comments.

**Your verdict:** ______

---

### D10. Risk: `Halt` marked `noinline` [§2.2.3]

**Concept.** See "Inlining, `noinline`, `cold`" in Part 0.

**Q1. The doc claims.** "When a sequence gap or the loss limit is detected, the application cancels every order and stops quoting. This happens at most once per run, so the handler that does it is marked `[[gnu::noinline]]`. Keeping it out of line keeps this rarely used code out of the quoting functions and their instruction-cache footprint."

**Q2. In the code.**
- `app/market_maker.h:43-45` (the declaration, with `[[gnu::noinline]]`)
- `app/market_maker.cpp:144-148` (the definition)
- Called from `market_maker.cpp:81` (inside `Requote`'s `catch`) and `market_maker.cpp:125` (`OnTimer`)

**Q3. On the hot path?** `Halt` itself runs at most once, so it's cold. What matters is whether it bloats `Requote`.

**Q4. Does the claim hold up?**
- *What's right in principle:* keeping rarely used code out of a hot function makes the hot code denser in the instruction cache, and stops the compiler from inlining the cancel logic into `Requote`.
- *Limits:* `Halt` is small (one log note and a `CancelAll`), and its call sits in a `catch` block that compilers generally already treat as unlikely (typical, not checked here). So the effect is probably small. `[[gnu::cold]]` would be the stronger tool, since it also moves the function away from hot code.
- *A subtlety:* "at most once" is for `Halt`. After it, `Requote` still throws from `CheckStanding` on every later message (see D9).
- *Tentative read:* sound reasoning with a small effect. Possible as a "good" decision if you're clear about the magnitude.

**Your verdict:** ______

---

### D11. Order management: decision log written before every send [§2.2.4]

**Concept.** A *write-ahead log* records what you intend to do before you do it. It's only useful if the record survives a crash. See "stdio buffering" in Part 0.

**Q1. The doc claims.** "Every order is written to a decision log (`decisions.log`) before it is sent. If the process dies mid-send, the log still shows what we intended to have on the exchange, so our state can be reconstructed on restart. Logging after the send would leave a window in which an order is live on the exchange with no record of it."

**Q2. In the code.**
- `app/execution/orders.cpp:13` (`log_.Record(...)`, before the `Send` calls at lines 21 and 30)
- `app/execution/decision_log.h:22` (`fopen` in `"w"` mode, so default stdio buffering), `decision_log.h:31-38` (`Record`, with the `fprintf` at 33-37)

**Q3. On the hot path?** Yes, directly. It's immediately before `Send` for every order, so it adds to every order's tick-to-trade, including the second order in a two-order callback.

**Q4. Does the claim hold up?**
- *The cost:* `fprintf` formats about seven fields into text (format parsing, integer-to-decimal conversion), takes the stdio lock, and copies into a buffer. When the buffer fills (about 4 KB, so roughly every 90 lines at about 45 bytes per line), one `write` system call runs, on whichever order happened to fill it. These are typical figures, not measured here.
- *The benefit isn't delivered:* `fprintf` writes into a buffer *inside the process*. If the process dies, the buffered lines are lost, so the log can be missing the last tens of orders, exactly the ones you would need. The write-ahead guarantee holds only if the record leaves the process before `Send`.
- *Measured here:* the first two sends in `basic` took 5,375 ns and 8,458 ns from entry, against 333–584 ns for later single-order callbacks. The first send also runs cold code, so I can't attribute this to the log alone. *Earlier measurement:* about 5 µs for the first sending message.
- *Possible fixes and trade-offs:*
  - Write a fixed-size **binary record** into a shared-memory file (`mmap` with `MAP_SHARED`). It's a `memcpy`, with no formatting, and the kernel owns the pages, so it survives a process crash. Cost: the format has to be decoded offline.
  - Hand records to a **background thread** through a lock-free ring buffer (the README allows other threads for non-`Send` work). Cost: records in the ring are lost if the process dies, unless the ring is in shared memory.
  - **Log after the send.** The cheapest, but it gives up the doc's guarantee.
  - `make verify` only compares `orders.txt`, so the log's format can change.
- *Tentative read:* a strong flaw: it is directly on the path, and the stated benefit isn't actually achieved by the code.

**Your verdict:** ______

---

### D12. Order management: unbounded quote history [§2.2.4]

**Concept.** A `std::vector` that grows by `push_back` has to occasionally allocate a bigger block, copy everything over, and free the old one. See "Heap allocation" in Part 0.

**Q1. The doc claims.** "Every quote we send is also appended to an in-memory history. The history is kept for the whole run, because it is needed for end-of-day reconciliation against the exchange's records; the timer checks the new entries as they accumulate."

**Q2. In the code.**
- `app/execution/orders.h:30-38` (`QuoteRecord`: about 40 bytes with padding), `orders.h:78` (`history_`)
- `app/execution/orders.cpp:12` (`history_.push_back`, before the log and the `Send`)
- `app/market_maker.cpp:134-142` (`Reconcile`), called from `OnTimer` at line 128

**Q3. On the hot path?** The `push_back` is on the order path, before `Send`. `Reconcile` runs on the timer, off the order path. (In this simulator a slow timer callback isn't counted in the next message's tap latency, because messages are delivered back to back. In a real system it would delay a message that arrived during it.)

**Q4. Does the claim hold up?**
- *Cost:* there is no `reserve`, so when the capacity is full the vector allocates a new block (typically double), copies every record, and frees the old block, inside the order path. Each order's latency spikes at those moments; later reallocations copy more data, and fresh memory causes page faults. The average cost per order is small, but the tail is not.
- *Purpose:* the doc says it's for reconciling against *the exchange's records*. `Reconcile` never reads the exchange's records. It only checks that the price is positive, a multiple of the tick, and the size is 100, which is true by construction (`RoundToTick` and `kQuoteShares`), so the check can't fire. The decision log already holds the same information.
- *Fix:* `reserve` the capacity up front (the app already does this for `LatencyLog`), or drop the history and the tautological check. Behavior is unchanged: the only output of `Reconcile` is a note on a mismatch that never happens.
- *Not yet verified:* the number of reallocations in a real run, which I haven't counted.
- *Overlap:* D11 and D12 are both about logging, so choosing both gives less variety.
- *Tentative read:* a flaw with a weak stated purpose and a spiky cost. Medium impact, easy fix.

**Your verdict:** ______

---

### D13. General: no cache warm-up [§2.2.5]

**Concept.** See "CPU cache" and "Warm-up" in Part 0. Code and data that haven't run recently may have been evicted from the fast caches, so the first use is slow.

**Q1. The doc claims.** "The application does not run any warm-up work to keep its caches primed. It is small enough that its code and data fit comfortably in cache, and nearly every message already runs the book and pricing code."

**Q2. In the code.** It's the absence of code. Related: the constructors that zero the large arrays once (`app/market_data/book.cpp:19-22`) and `LatencyLog`'s `reserve` (`latency_log.h:28`). Nothing in `main.cpp` or `market_maker.cpp` warms anything.

**Q3. On the hot path?** Cache state affects every callback, but it matters most for the rarely run send path.

**Q4. Does the claim hold up?**
- *What's right:* the book and pricing code run on every message, so they stay warm.
- *What's missing:* the **send path** (`SendQuote`, the `fprintf`, the encode, `Send`) runs only when a quote changes, 11 of 54 callbacks in `basic`, and that's the path whose latency matters. It can be evicted between uses.
- *Also:* the simulator itself (event queue, exchange matching, capture) runs on the same core between callbacks and competes for cache. And "data fits comfortably in cache" is true only for the touched part: the allocated tables are 16 MB (orders) plus 3.2 MB (ladders), bigger than a typical L2.
- *Measured here:* the first sends in `basic` took 5.4 µs and 8.5 µs, against 0.3–0.6 µs later. But the first callback also has first-use effects (the first `fprintf`, the first call of each function), so this isn't proof of cache eviction.
- *Fix idea:* run the send path against a stub during idle time (no real `Send`, no token consumed). Caveats: the simulator delivers messages back to back, so there is little idle time, and warm-up done after `Record()` could flatter the results.
- *Tentative read:* arguable, with weak evidence so far. It would be hard to defend in an interview without isolating the cause.

**Your verdict:** ______

---

## Part 3: Summary

| # | Decision | Hot path? | Claim holds? | Tentative read | What would settle it |
|---|---|---|---|---|---|
| D1 | Average over all messages | Metric, not the path | No (wrong population, endpoint, mean) | Clear flaw | Recompute from `capture.csv` on a longer run |
| D2 | Flat array by ref | Yes | Yes, with limits (1M refs, 16 MB) | Good | Needs no more |
| D3 | Price ladder | Yes | Yes | Good | Needs no more |
| D4 | Cached top of book | Yes | Yes, except the scan assumption | Good, one weak spot | Wide-gap scenario |
| D5 | `memcpy` decode | Yes | Yes (small effect) | Good, minor | Needs no more |
| D6 | Integer prices | Yes | Mostly ("throughout" overstated) | Good, minor | Needs no more |
| D7 | Credit every message | Yes | No ("latest" is vacuous) | Flaw, small, safe fix | Micro-benchmark |
| D8 | One function, both sides | Yes | Yes | Neutral | Check inlining |
| D9 | Exceptions for risk | Yes (the throws too) | First half yes, second half no | Strongest flaw | Count throws, time one |
| D10 | `noinline` Halt | Cold | Yes, small effect | Good, minor | Needs no more |
| D11 | Decision log before send | Yes, directly | No (the buffering defeats the benefit) | Strong flaw | Time with logging off |
| D12 | Unbounded quote history | `push_back` yes | Weak purpose | Flaw, medium | Count reallocations |
| D13 | No warm-up | Affects the send path | Partly | Arguable, weak evidence | Isolate the first-send cost |

**What the critique needs:** one flaw in the evaluation (D1 is the only evaluation decision, so the choice is which angle of it), one good decision, two flaws in the market maker, and one hot-path execution. The choices are yours. D11 and D12 overlap, and D2/D3/D4/D5/D6/D10 are the good-decision candidates.

## Caveats

- The "tentative read" column is my reading of the code and the evidence so far, and I could be wrong. Your own verdict lines matter more.
- I haven't read `sim/exchange/*`, so claims about ref ordering and queue priority rest on the trace.
- All figures marked "typical" are general knowledge. None of the micro-costs (exception, `fprintf`, `sqrt`, reallocation) have been isolated by measurement yet.