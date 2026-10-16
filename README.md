# Market maker simulation

A single-process simulation of an electronic exchange and one trading
application connected to it.

- `sim/` is the fixed environment: the exchange, message formats, and replay
  runtime. It is not the system under analysis, so it is written for clarity
  rather than speed. Don't modify it.
- `app/` is the trading application: the system under analysis. You may
  change how it works, but not what it does (see "Checking behavior" below).

## Building

Requires a C++20 compiler (g++ 11+ or clang 14+) and `make`. There are no
other dependencies.

```sh
make                                        # builds everything into build/
make test                                   # runs the simulator tests
./build/sim_replay sim/fixtures/basic.scn   # prints the message trace for a scenario
```

## Running the application

```sh
./build/market_maker sim/fixtures/sweep.scn            # outputs go to build/out/sweep/
./build/market_maker sim/fixtures/sweep.scn --out DIR  # ... or to DIR
./build/market_maker sim/fixtures/sweep.scn --trace    # also write the full message trace
```

Each run prints the application's latency report and writes:

| File | Contents |
|---|---|
| `capture.csv` | The capture tap: every delivery and every order sent, with wall-clock times (see below) |
| `latency_log.csv` | The application's own record of when each callback finished |
| `orders.txt` | Every order the application sent, with its simulated time |
| `decisions.log` | The application's decision log |
| `trace.txt` | With `--trace` only: every message, human-readable. Tracing is slow, so don't measure with it |

## Checking behavior

The application's trading behavior is fixed: for every scenario, it must
send exactly the same orders (same times, prices, sizes, and tokens). Each
scenario in `sim/fixtures/` that has a `.expected` file records that order
stream.

```sh
make verify    # runs every fixture and diffs its orders against .expected
```

A failing fixture leaves its diff in `build/verify/<name>/diff.txt`.

## How the simulation runs

Everything runs in one process, on one thread, against a **simulated clock**.
Nothing goes over a network. Every message is placed on a time-ordered event
queue and delivered after a configurable one-way latency:

```
scripted participants ───────────(0 ns)────────────▶ ┌──────────┐
                                                     │          │
application ── OrderEntrySession::Send ─(order_entry)▶│ exchange │
            ◀── OnExecutionReport ──────(order_entry)─│          │
            ◀── OnMarketData ───────────(market_data)─└──────────┘
```

- Simulated time doesn't advance while the application is inside a
  callback.
- Events with the same timestamp are delivered in the order they were
  scheduled.
- A given scenario therefore always produces exactly the same run.

### The application interface

The application is any type with these three methods (checked by the
`sim::Application` concept in [sim/runtime/simulator.h](sim/runtime/simulator.h)):

```cpp
void OnMarketData(std::span<const std::byte> message);      // one feed message
void OnExecutionReport(std::span<const std::byte> message); // one report for our orders
void OnTimer(sim::Timestamp now);                           // every `timer` ns, if configured
```

Callbacks receive **raw wire bytes**. Decoding them is the application's job.
The application sends orders through the `sim::OrderEntrySession` it gets
from the simulator:

```cpp
sim::Simulator simulator(sim::LoadScenario("sim/fixtures/basic.scn"));
MyApp app(simulator.session());   // app keeps the session to call Send(...)
simulator.Run(app);
```

`Send` may only be called on the thread running `Simulator::Run`, i.e. from
inside one of the callbacks above. You may use other threads for other work,
but calling `Send` from one of them throws `std::logic_error`. The simulator
isn't thread-safe, and each order is attributed to the message being handled
when it was sent.

### Capture tap ([capture.h](sim/runtime/capture.h))

The simulator can record everything that crosses the sim/app boundary, like
a network capture taken at a switch:

```cpp
sim::Capture capture;
simulator.set_capture(&capture);
simulator.Run(app);
sim::WriteCaptureCsv(capture, csv_file);
```

- For every delivery (market data message, execution report, timer tick):
  its simulated time and the wall-clock time the callback was entered.
- For every `Send`: the wall-clock time, and the delivery being handled when
  it was called.

The tap does **not** record when callbacks return. Wall-clock times come
from `sim::WallClockNow()` ([clock.h](sim/runtime/clock.h)). Use the same
function for any timestamps your application takes, so they're comparable
with the tap's. Each tap timestamp costs roughly 20 ns on the application's
thread.

Messages are delivered back to back: the simulator does not wait for wall
time to catch up with simulated time, and simulated time stands still while
a callback runs.

## Protocols

All messages are packed structs with fixed field offsets, little-endian.
Prices are fixed-point integers with 4 implied decimals ($100.25 = 1002500),
and the tick size is $0.01. The first byte of every message is its type.

### Market data feed: exchange → everyone ([market_data.h](sim/messages/market_data.h))

The feed is ITCH-like and order-by-order. Every message carries a sequence
number that increases by exactly 1, plus the exchange timestamp.

| Type | Message       | Meaning                                              |
|------|---------------|------------------------------------------------------|
| `A`  | AddOrder      | New resting order: ref, side, shares, price          |
| `E`  | OrderExecuted | Resting order `ref` traded `shares` (the trade print) |
| `X`  | OrderCancel   | Resting order `ref` reduced by `shares`              |
| `D`  | OrderDelete   | Resting order `ref` removed                          |
| `U`  | OrderReplace  | Order `ref` replaced by `new_ref` at a new price/size |

Only `A` and `U` carry a price and side. To know which price level an
`E`, `X` or `D` affects, a consumer has to remember the orders it has seen.
The feed includes your own resting orders, and it doesn't say which orders
are yours.

### Order entry: application → exchange ([order_entry.h](sim/messages/order_entry.h))

Order entry is OUCH-like and unsequenced. You identify your orders with
tokens that you choose, and a token can be used only once. An `EnterOrder`
with a token that has already been used is silently ignored, so resending is
safe.

| Type | Message      | Meaning                                                   |
|------|--------------|-----------------------------------------------------------|
| `O`  | EnterOrder   | New limit order, `Day` or `ImmediateOrCancel`             |
| `U`  | ReplaceOrder | Replace a live order with a new token, price and size. Loses priority. |
| `X`  | CancelOrder  | Reduce a live order to `shares`. 0 cancels it.            |

### Execution reports: exchange → order owner ([execution_report.h](sim/messages/execution_report.h))

Execution reports are sequenced per session, starting at 1.

| Type | Message  | Meaning                                                        |
|------|----------|----------------------------------------------------------------|
| `A`  | Accepted | Order is live. Carries the exchange-assigned `order_ref`.      |
| `U`  | Replaced | Replacement is live. Carries its new `order_ref`.              |
| `E`  | Executed | Your order traded. `liquidity` says whether you rested or took. |
| `J`  | Rejected | Order refused (bad price, bad size, unknown order to replace)  |
| `C`  | Canceled | Shares removed, either at your request or as an IOC remainder  |

The exchange assigns the `order_ref` when it processes your order. You only
learn it when `Accepted` arrives, one full round trip after you sent the
order.

## Scenarios

A scenario file scripts the order flow from other participants and sets the
simulator parameters. The format is documented at the top of
[sim/replay/scenario.h](sim/replay/scenario.h), and
[sim/fixtures/basic.scn](sim/fixtures/basic.scn) is an example.
