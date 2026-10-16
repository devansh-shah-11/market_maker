# Block-by-block walkthrough of the `basic` run

Source: `build/out/basic/trace.txt`. All times are simulated nanoseconds.
The fair-value and desired-price numbers come from a replay of the app's logic over the trace (scratch script, not in the repo). The replay reproduces all 14 real orders in `orders.txt` with the same times and prices.
Block numbers 1 to 8 match the chat explanation; Blocks 0a and 0b cover the startup lines before 500,000.

## Terms

- **Believed book:** the order book as our app sees it. It's built from the feed, with our own orders removed once we know they're ours.
- **Fair value (fv):** the microprice, a size-weighted price of the best bid and ask: `(bid × askSize + ask × bidSize) / (bidSize + askSize)`. It leans toward the side with *less* size, because that side is more likely to be used up first. Our quotes are `fv − 1¢` (rounded down) for the bid and `fv + 1¢` (rounded up) for the ask.
- **Token:** an order ID that a participant chooses. **Ref:** the ID the exchange assigns to every order.
- **Pending:** we've sent a request for that side and are waiting for the reply. While a side is pending, we send nothing more on it.
- **Requote:** after every feed message and every report, the app recomputes both desired prices. For each side that isn't pending, it sends a Replace or Enter only if the desired price differs from the resting one.
- **IOC (immediate-or-cancel):** trade what you can right now and cancel the rest. **Day order:** can rest on the book.

**Timing rule:**
- An outside participant's order reaches the exchange at time T. We see it in the feed at **T + 5,000**.
- Our order reaches the exchange at T + 10,000. The feed shows it at T + **15,000**, and our report arrives at T + **20,000**.

## What a timer tick is

A timer tick is a wake-up call from the simulator. It isn't a market event, and it doesn't come from the exchange.

The scenario file says `timer 1_000_000`, so every 1,000,000 ns (1 ms) of simulated time the simulator calls the app's `OnTimer`. In `basic` that's 10 ticks, from 1,000,000 to 10,000,000.

`OnTimer` does four jobs, and **none of them sends an order**:
1. Samples the **mid price**: `(best bid + best ask) / 2`, using the believed book.
2. Updates the **volatility** estimate with that sample.
3. Checks the **loss limit**. P&L is cash plus position × mid, and if it falls below −$100 the app halts.
4. Reconciles the quote history. This is a check of the quotes sent since the last tick.

## What volatility is, and why a market maker cares

Volatility measures how much the price has been moving. It's a risk signal, not a price.

A market maker quotes a bid and an ask and earns the spread. The danger is that someone who knows the price is about to move trades with you just before it does. That's called being "picked off". When the price is jumpy, you ask for more room, so you quote further from fair value. That extra distance is the **credit** (half-spread), and it widens with volatility:

`credit = max(1¢, 2 × σ × mid)`

**How the app computes σ** (the formula is fixed by the spec):
1. Take the mid at each tick. Compute the **log return**: `r = ln(new_mid / old_mid)`. It's roughly the percentage change.
2. Update a running estimate with an exponentially weighted average: `variance = 0.94 × old_variance + 0.06 × r²`. Recent moves count most, and older moves fade at 6% per tick.
3. σ is the square root of that variance.

**The `basic` numbers:**

| Timer | Mid sampled | r² | Variance | σ | 2σ·mid |
|---|---|---|---|---|---|
| 1,000,000 | 100.025 | none (first sample) | 0 | 0 | $0 |
| 2,000,000 | 100.03 | 2.5e-9 | 1.5e-10 | 0.0012% | **$0.0025** |
| 3,000,000 | 100.025 | 2.5e-9 | 2.9e-10 | 0.0017% | $0.0034 |
| 6,000,000 | 100.055 | 6.2e-8 | 4.1e-9 | 0.0064% | **$0.0129** |

The credit has a floor of $0.01 (`kMinCredit`). Until the very last ticks, 2σ·mid is below 1¢, so the floor wins and the credit is 1¢ in every requote. The first time the formula gives more than 1¢ is at 6,000,000, after the jump to 100.055 at 5,005,000. But no feed messages or reports arrive after that, so no requote happens and the wider credit is never used.

**Why sample on the timer?** The design doc says it's a modelling choice: per-message returns are dominated by the price bouncing between bid and ask. Sampling at a fixed horizon makes the estimate mean the same thing no matter how many messages arrive.

---

## Block 0a: Startup (t = 0 to 5,000)

**Starting state:** nothing. The market doesn't exist yet, and the app has received nothing.

| Time | What happens |
|---|---|
| 0 | Four scripted orders reach the exchange with zero latency: p2 bids 99.99 × 200 and 100.00 × 300, p3 asks 100.05 × 300 and 100.06 × 200. The exchange gives them refs 1 to 4 and publishes four `ADD` messages. |
| 5,000 | The four feed messages reach the app one after another (5,000 latency). |
| | `ADD ref=1` (bid 99.99): only a bid exists, so the book is invalid. The app skips the tick. This is the `BookInvalid` exception path. |
| | `ADD ref=2` (bid 100.00): still no ask. Same skip. |
| | `ADD ref=3` (ask 100.05): both sides exist. Bid 100.00 × 300, ask 100.05 × 300, so fv = 100.025. Desired bid = 100.015, rounded down to **100.01**. Desired ask = 100.035, rounded up to **100.04**. No resting orders, so the app sends **`ENTER token 1 BUY @ 100.01`** and **`ENTER token 2 SELL @ 100.04`**. |
| | `ADD ref=4` (ask 100.06): doesn't change the best ask, and both sides are pending. No action. |

**End state:** our bid and ask are in flight. The spread is 5¢ wide and ours is 3¢ wide, inside it.

## Block 0b: Our own orders appear in the feed, and the quote flickers (t = 20,000 to 65,000)

**Starting state:** both Enters are in flight. They reach the exchange at 15,000 and get refs 5 (bid) and 6 (ask).

| Time | What happens |
|---|---|
| 20,000 | The feed shows `ADD ref=5 BUY 100 @ 100.01` and `ADD ref=6 SELL 100 @ 100.04`. These are our orders, but the feed doesn't say so and the app doesn't know yet, so it counts them as market liquidity. Both sides are pending, so no action. |
| 25,000 | **Two reports arrive one after the other.** |
| | `ACCEPTED token 1, ref 5`: the app learns ref 5 is ours and removes it from the believed book, so the market bid is 100.00 × **300**. But report #2 hasn't been processed yet, so our ask (ref 6, 100.04 × 100) is still counted as market liquidity. Believed book: bid 100.00 × 300, ask 100.04 × **100**. fv = `(100.00×100 + 100.04×300)/400 = 100.03`. Desired bid = **100.02**. The bid is live at 100.01 and not pending, so the app sends **`REPLACE token 1 → 3 @ 100.02`**. |
| | `ACCEPTED token 2, ref 6`: now the ask is known as ours too. The book is the true market (100.00 × 300 / 100.05 × 300), fv = 100.025, desired ask 100.04 = resting. No action (the bid is pending anyway). |
| 40,000 | The feed shows our replace (ref 5 → 7). The app sees it replacing one of its own orders, so ref 7 is marked ours immediately. No action. |
| 45,000 | `REPLACED token 1 → 3`: the bid is live at 100.02. The true book gives desired bid 100.01, so the app sends **`REPLACE token 3 → 4 @ 100.01`**. |
| 60,000 / 65,000 | Feed and report confirm. The bid is live at 100.01. |

**End state:** bid 100.01 and ask 100.04, exactly where we started. Two wasted order messages.

**Why:** the 100.02 was never a real signal. For one callback, our own ask looked like thin market liquidity. The strategy spec requires this behavior, and `make verify` expects it, so it can't be removed.

---

## Block 1: A quiet competitor (t = 500,000 to 1,000,000)

**Starting state:** our bid is live at 100.01 (token 4, ref 8) and our ask at 100.04 (token 2, ref 6). Market bids: 99.99 × 200 and 100.00 × 300. Market asks: 100.05 × 300 and 100.06 × 200.

| Time | What happens |
|---|---|
| 500,000 | p3 enters a bid of 100 at 100.00, which joins p2's 100.00 bid. |
| 505,000 | The feed delivers it. The best bid is now 100.00 × **400**. fv = 100.0285. The desired prices are bid 100.01 and ask 100.04, which equal what we already have, so the app **does nothing**. |
| 1,000,000 | Timer tick. The mid is `(100.00 + 100.05)/2 = 100.025`. It's the first sample, so there's no return yet and the volatility is 0. The credit stays at the 1¢ minimum. The timer **never sends orders**. |

**End state:** nothing changed. Most messages in a real run look like this.

## Block 2: A competitor improves the bid, and the ask follows (t = 1,200,000)

**Starting state:** bid 100.01, ask 100.04. Market bids: 100.00 × 400, 99.99 × 200. Market asks: 100.05 × 300, 100.06 × 200.

| Time | What happens |
|---|---|
| 1,200,000 | p2 enters a bid of 100 at **100.01**, the new best bid. |
| 1,205,000 | The feed delivers it. Our own 100.01 bid is excluded from the book, so the only visible best bid is p2's new one, 100.01 × **100**, which is thin. The best ask is 100.05 × 300. So fv = `(100.01×300 + 100.05×100)/400 = 100.02`. The desired bid is 100.01, which matches. The desired ask is 100.03, but the resting ask is 100.04, so the app sends **`REPLACE token 2 → 5 @ 100.03`**. |
| 1,220,000 | The feed shows our replace (ref 6 → 11). |
| 1,225,000 | The report says it's live. The ask is now 100.03. |

**End state:** bid 100.01, ask 100.03.

**Why:** a thin bid next to a thick ask pulls fair value down, so the ask follows it down.

## Block 3: A sell sweep fills our bid (t = 2,000,000)

**Starting state:** our bid is live at 100.01 (token 4, ref 8) and our ask at 100.03 (token 5, ref 11). Market bids: 100.01 × 100 (p2, ref 10), 100.00 × 400, 99.99 × 200. Market asks: 100.05 × 300, 100.06 × 200. Position 0.

p4 sends a sell for 250 shares at 100.00 as **IOC**. It hits bids in price order, then time order:
1. Our bid ref 8 (100.01 × 100) is first, so we're **filled for 100**.
2. p2's bid ref 10 (100.01 × 100) is next.
3. Then 50 of p2's ref 2 at 100.00.

| Time | What happens |
|---|---|
| 2,000,000 | Timer tick. It samples the mid at 100.03 (bid 100.01, ask 100.05). The volatility goes slightly above zero, but 2σ·mid is only about $0.0025, so the credit is still 1¢. |
| 2,005,000 | **Three feed messages arrive at the same instant, and the app handles them one at a time.** |
| | `EXECUTED ref=8`: our own order. We exclude it, so nothing changes in the believed book and the app does nothing. |
| | `EXECUTED ref=10`: p2's bid is gone. The best bid drops to 100.00 × **400**. fv = 100.0285 and the desired ask is 100.04. The resting ask is 100.03, so the app sends **`REPLACE token 5 → 6 @ 100.04`**. |
| | `EXECUTED ref=2 50`: the book shifts a little, but the ask is pending, so nothing is sent. |
| 2,010,000 | **We learn we were filled.** `EXECUTED token 4, 100 @ 100.01` arrives, 10,000 after the sweep. The position is now **+100**. Our bid has no shares left, so no bid is resting. The app sends **`ENTER token 7 BUY @ 100.01`**. |

**End state:** position +100, a new bid in flight, an ask replace in flight.

**Why:** the app reacted to the feed (2,005,000) *before* the fill report (2,010,000) because the feed is faster, 5,000 versus 10,000. It acted on part of the sweep before knowing about our fill.

## Block 4: The flicker, now on the ask (t = 2,020,000 to 2,065,000)

**Starting state:** the bid Enter (token 7) and the ask Replace (token 6) are in flight. Position +100.

| Time | What happens |
|---|---|
| 2,020,000 | The feed shows our ask replace (ref 11 → 13 @ 100.04). |
| 2,025,000 | Two things arrive. First, the report says the ask replace is live at 100.04. The true book gives fv = 100.0269, whose desired ask is 100.04, so nothing is sent. Second, the feed adds ref 14, a bid of 100.01 × 100. That is **our own new bid** (token 7), but the app doesn't know yet, so it counts as a thin market bid. fv drops to 100.02, the desired ask becomes 100.03, and the app sends **`REPLACE token 6 → 8 @ 100.03`**. |
| 2,030,000 | `ACCEPTED token 7 ref 14`. Now the app knows ref 14 is ours and removes it from the book. The bid is live at 100.01, so no action. |
| 2,040,000 / 2,045,000 | The feed shows the replace (ref 13 → 15). The report says it's live. The book is true again, fv = 100.0269, and the desired ask is 100.04. The app sends **`REPLACE token 8 → 9 @ 100.04`**. |
| 2,060,000 / 2,065,000 | Feed and report confirm. The ask is back at 100.04. |

**End state:** bid 100.01, ask 100.04.

**Why:** the same mechanism as Block 0b. The two replaces move the ask down and then back up. They are wasted messages caused by the 5,000 delay between "our order is in the feed" and "we've been told it's ours".

## Block 5: A thin ask moves both quotes (t = 3,000,000 to 3,105,000)

**Starting state:** bid 100.01 (token 7, ref 14), ask 100.04 (token 9, ref 16). Market bids: 100.00 × 350, 99.99 × 200. Market asks: 100.05 × 300, 100.06 × 200.

| Time | What happens |
|---|---|
| 3,000,000 | p3 cancels 200 of its 300-share ask at 100.05, leaving 100. |
| 3,005,000 | The feed shows `CANCEL ref=3 −200`. The market asks are now 100.05 × **100** against bids of 100.00 × 350. fv = `(100.00×100 + 100.05×350)/450 = 100.0388`. The thin ask means the price is likely to go up. The desired bid is 100.02 and the desired ask is 100.05. The resting orders are 100.01 and 100.04, and neither side is pending, so **two orders go out from one message**: **`REPLACE token 7 → 10 @ 100.02`** and **`REPLACE token 9 → 11 @ 100.05`**. |
| 3,020,000 / 3,025,000 | The feed and the reports confirm both. |
| 3,100,000 / 3,105,000 | p3 cancels another order, a bid of 100 at 100.00, and the feed shows a `DELETE`. The best bid stays at 100.00 but its size falls to 250. fv = 100.0357, the desired prices are still 100.02 and 100.05, so **no action**. |

**End state:** bid 100.02, ask 100.05.

## Block 6: A competitor changes its order (t = 4,000,000)

**Starting state:** bid 100.02 (token 10, ref 17), ask 100.05 (token 11, ref 18). Market bids: 100.00 × 250, 99.99 × 200. Market asks: 100.05 × 100, 100.06 × 200.

| Time | What happens |
|---|---|
| 4,000,000 | p2 replaces its order (ref 2, 250 shares at 100.00) with 150 shares at **100.01**. It gets a new ref (19) and loses its place in the queue. |
| 4,005,000 | The feed shows it. The book is now bids 100.01 × 150 and asks 100.05 × 100, so fv = 100.034. The desired prices are bid 100.02 and ask 100.05, which are still what we have, so the app sends nothing. |

**End state:** unchanged. The market moved, but not enough to change our rounded prices.

## Block 7: A buy sweep fills our ask, and our Replace is rejected (t = 5,000,000)

**Starting state** (just before t = 5,000,000)

| | Real exchange book | What the app believes (ours excluded) |
|---|---|---|
| Bids | 100.02 × 100 (ours, token 10, ref 17); 100.01 × 150 (p2); 99.99 × 200 | 100.01 × 150; 99.99 × 200 |
| Asks | 100.05 × 100 (p3, ref 3); 100.05 × 100 (ours, token 11, ref 18); 100.06 × 200 | 100.05 × 100; 100.06 × 200 |

Our quotes are bid 100.02 and ask 100.05, and the position is +100 from the fill in Block 3.

**5,000,000: the aggressive order.** p4 sends a BUY for 400 shares at 100.05, a Day order. At 100.05 it's "marketable", because 100.05 is the price of the best ask. The exchange matches it immediately, in price order, then time order within a price:
1. p3's ask ref 3 (100 shares), which has been queued since t = 0.
2. **Our** ask ref 18 (100 shares). This is the *replacement* we created at 3,020,000, so it lost its place and sits behind ref 3. The trace shows this order of execution.
3. That's 200 filled. The other 200 shares stay unfilled. Because it's a Day order (not IOC), they rest as a **new bid at 100.05** (ref 20).

This all happens in zero time at the exchange. After it, the real exchange has no asks at 100.05, a new best bid of 100.05 × 200, and a best ask of 100.06.

**What the app sees, in the order it sees it:**

| Time | Message | What the app does |
|---|---|---|
| 5,005,000 | Feed: `EXECUTED ref=3` (100) | p3's ask is gone, so the believed best ask moves to 100.06 × 200. Believed book: bid 100.01 × 150, ask 100.06 × 200. fv = (100.01×200 + 100.06×150)/350 = 100.0314. Desired bid 100.02, desired ask 100.05. Both equal the resting prices, so **no action**. |
| 5,005,000 | Feed: `EXECUTED ref=18` (100) | This is our own order. The app updates its order table but excludes it from the book, so nothing changes. **No action.** The app does not yet know that we were filled. |
| 5,005,000 | Feed: `ADD ref=20 BUY 200 @ 100.05` | The best bid is now 100.05 × 200 and the best ask is 100.06 × 200. fv = 100.055 exactly. Desired bid = 100.045, rounded down to **100.04**. Desired ask = 100.065, rounded up to **100.07**. Neither matches what the app thinks is resting (100.02 and 100.05), and neither side is pending. So it sends **both**: `REPLACE token 10 → 12 @ 100.04` and `REPLACE token 11 → 13 @ 100.07`. |

**The mistake in that last step:** the app believes its ask (token 11) is still resting, but it was filled at t = 5,000,000. The app had no way to learn this in time from the report, because the report takes 10,000 to arrive.

**5,010,000: the fill report.** `EXECUTED token=11, 100 @ 100.05, ADDED` arrives. "ADDED" means our order was resting when it traded, so we provided the liquidity. The app updates:
- Position: +100 − 100 = **0**.
- Cash: bought 100 at 100.01 and sold 100 at 100.05, so **+$4.00**. That's the 4¢ of spread on 100 shares.
- The ask has no shares left, so the app marks it not live.

The app then re-evaluates, but both sides are **pending** (the two Replaces are in flight), so it sends nothing. This guard stops it from double-sending.

**5,015,000: invisible in the trace.** Our two Replaces reach the exchange. The trace only shows messages sent and received by the app, so this step has no line.
- The bid replace works: the order is alive, so it's replaced at 100.04 with a new ref, 21.
- The ask replace **fails**: token 11 was fully filled 15,000 ago, so there's nothing to replace.

**5,020,000 to 5,045,000**

| Time | Message | What the app does |
|---|---|---|
| 5,020,000 | Feed: `REPLACE ref=17 → 21 @ 100.04` | Our bid's replacement. The app already knew ref 17 was ours, so it marks ref 21 as ours at once. No action (bid still pending). |
| 5,025,000 | Report: `REPLACED token 10 → 12, ref 21` | The bid is live at 100.04. The desired bid is 100.04, so no action. |
| 5,025,000 | Report: **`REJECTED token 13, reason D`** | The exchange refused the ask Replace. The app clears the ask's pending flag. The ask now has no resting order and no request in flight, so it sends a **new** order: `ENTER token 14 SELL @ 100.07`. It's an `ENTER` and not a `REPLACE` because there's nothing to replace. |
| 5,040,000 | Feed: `ADD ref=22 SELL 100 @ 100.07` | Our new ask, not yet known as ours. It's counted as market, but 100.07 sits behind the best ask of 100.06, so it doesn't change the top of the book. No flicker this time. |
| 5,045,000 | Report: `ACCEPTED token 14, ref 22` | The app now knows ref 22 is ours. The ask is live at 100.07. |

**End state:** bid 100.04 and ask 100.07, around fair value 100.055. Position 0, +$4.00 earned. We went from 100.05/100.02 to 100.07/100.04 as the market moved up.

**Why this block matters for the critique:** the rejection is not a bug. It's the unavoidable result of acting on information that is 10,000 old, and the spec allows for it. The cost of the extra round trip comes from the network latency, not from our code, so it's outside what the assignment asks you to optimize.

## Block 8: The end (t = 6,000,000 to 10,000,000)

**Starting state:** bid 100.04, ask 100.07. No orders in flight.

Only timer ticks. The mid is 100.055 (bid 100.05, ask 100.06). After the 5,005,000 price jump the volatility estimate is non-zero, so the credit the app *would* use is $0.0129, $0.0125, $0.0121, $0.0118 and $0.0114 at the five ticks. But the timer never requotes, and no feed messages or reports arrive, so no orders go out. The run ends.

---

## Caveats

- The fv and desired-price numbers come from my replay of the app's logic, not from the app's own output. The replay reproduces all 14 real orders, which is why I trust it.
- The queue-priority orders (Block 3: ref 8 before ref 10 before ref 2; Block 7: ref 3 before ref 18) are read from the order of `EXECUTED` messages in the trace, not from the exchange code, which I haven't read yet.
- Block 7's 5,015,000 step is inferred from the reports. The trace has no line for orders arriving at the exchange.