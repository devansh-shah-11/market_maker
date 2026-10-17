# Critique draft: evaluation section (starting point, to be rewritten in your own words)

Only the "bad design decision in the evaluation" part is drafted here. The other three parts (hot path summary, one good market_maker decision, two bad market_maker decisions) are not drafted yet.

---

## Bad design decision in the evaluation: averaging handler time over every message (§2.1)

**The decision.** The latency report joins `LatencyLog`'s handler-finish time with the capture tap's arrival time for every message the app receives (market data, execution reports and timer ticks) and prints the average. The design doc justifies this by saying that most ticks send nothing, so the average should "account for all of that work".

**Why it is a bad choice.** The goal of the assignment is tick-to-trade: the time from a tick arriving to our order leaving. The report measures something else.

1. **It averages over the wrong messages.** Only messages that cause an order have a tick-to-trade. In `basic`, 54 messages arrive and 11 of them send (14 orders in total); the other 43, including 10 timer ticks that can never send, dilute the number. The result describes "average work per message", not how fast we trade.
2. **It stops at the wrong time.** The log stamps when the handler returns, not when the order leaves. A callback that sends two orders (bid, then ask) gets one number stamped after both. The bid left earlier than that, and the ask waited for the bid's send work.
3. **The mean hides the tail.** The p99 is 20 to 40 times the median on every fixture, so a mean says little about the slow orders that cost queue position.
4. **It is redundant.** The tap already records each order's send time and the delivery that caused it, outside our code. `send.time − delivery entry` gives the exact tick-to-trade per order with no `LatencyLog` at all.

**Evidence** (30 runs per fixture, orders from all runs pooled; times in ns; see "How we measured" below):

| fixture | callbacks that sent / all | current report (mean) | tick-to-trade median | p99 | max |
|---|---|---|---|---|---|
| basic | 11 / 54 | 2,998 | 666 | 22,917 | 25,875 |
| quiet | 1 / 48 | 3,095 | 15,229 | 27,500 | 27,500 |
| gap | 2 / 36 | 6,931 | 12,646 | 63,959 | 67,667 |
| position | 19 / 117 | 2,436 | 667 | 27,042 | 38,292 |

- On `quiet`, only 1 of 48 messages sends, so the report says about 3,100 ns while a typical order takes about 15,200 ns (about 5x more). The report understates tick-to-trade where few callbacks send.
- On `basic` and `position` the two means happen to land near each other, but the typical order (median) is 4 to 5x faster than the mean, and the slowest 1% are 30 to 40x slower. The report's single number matches neither.
- The report's own number is unstable: for `basic` it came out at about 856 ns when runs were launched back to back and about 3,000 ns with a short pause between runs.

**Proposed correction.**
- Stop using `LatencyLog` for the headline. For every order sent, report `send.time − entry of the causing delivery`, taken from the capture tap.
- Report the whole distribution (count, mean, median, p99, max), not just the mean. Callbacks that send nothing contribute no sample, which is intended because they have no tick-to-trade.
- Keep the cold first order in the data: it is a real order.
- Optionally report first order vs second order within a callback to show the cost of sending serially (median about 2x higher for the second order).
- Cost of the current log, for context: the join, average and printing happen after the run, so they cost nothing on the hot path. `Record` runs at the end of each callback (one clock read, about 20 ns, plus a `push_back` into reserved memory), after any `Send`, so it cannot delay an order in its own callback. We did not measure the saving from removing it, so we do not claim one.

---

## How we measured, what we measured, what we inferred

**How we measured.**
- Script: `tools/latency_compare.py` (outside `app/` and `sim/`; nothing in the app or the fixtures was changed, and `make verify` passed before we started).
- For each of the 7 fixtures in `sim/fixtures/` it ran exactly `./build/market_maker sim/fixtures/<name>.scn` 30 times, sleeping a random 300 to 800 ms before each run so the CPU is not kept hot.
- "Current report" = the `average latency:` line the binary prints, averaged over the 30 runs. This figure uses `LatencyLog`. The script does not recompute it.
- Tick-to-trade = for every order in `capture.csv`, the send's `wall_ns` minus the `wall_ns` of the delivery named in the send record. Orders from all 30 runs of a fixture are pooled into one list, sorted, and the median, p99 (nearest rank) and max are read from it. No run was dropped.

**What we measured.**
- The printed average for each fixture (mean of 30 runs).
- Tick-to-trade for every order: mean, median, p99, max, and first vs second order within a callback.
- Counts of messages, timer ticks, sending callbacks and orders per fixture.

**What we inferred (from those measurements, not measured directly).**
- The report does not track tick-to-trade: it is dominated by messages that send nothing and by a few slow orders.
- The first order of a fresh process is slow (cold code and caches). Every run is a fresh process, so each run adds a cold order to the pool, which probably explains most of the tail. We did not isolate the cause.
- `LatencyLog` costs nothing on the hot path through the report and very little through `Record`. That comes from reading the code and the README's 20 ns clock figure, not from a before/after test.

**Not measured / limits.**
- Removing `LatencyLog` and rerunning. No saving is claimed.
- Percentiles on small fixtures are coarse (`quiet` pools only 60 orders, so p99 equals the max).
- The clock steps in about 41 ns on this Mac, so differences below roughly 100 ns are not meaningful.
- Absolute times depend on how runs are launched (back to back vs with pauses), so only compare numbers taken the same way. All results are from one machine (macOS).
- One extreme outlier (211 µs in `loss`) is included.
