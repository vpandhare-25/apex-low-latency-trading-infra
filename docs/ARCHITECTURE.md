# Apex architecture

```
 NASDAQ ITCH 5.0 file
        │  (binary messages: add / execute / cancel / delete / replace)
        ▼
 ┌──────────────────────┐
 │ Market Data Handler  │  decodes big-endian binary into C++ structs
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │ Order Book Manager   │  one OrderBook per venue: price -> total shares
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐      ┌────────────┐
 │ Smart Order Router   │ ───▶ │ Venue A    │  50% of real orders, $0.0030 fee, 50 us
 │ (split parent order  │ ───▶ │ Venue B    │  20% of real orders, $0.0010 fee, 20 us
 │  by fee-adjusted     │ ───▶ │ Venue C    │  30% of real orders, $0.0020 fee, 1 ms
 │  price)              │      └────────────┘
 └──────────┬───────────┘
            ▼
 ┌──────────────────────┐
 │ Execution Analytics  │  avg price, slippage, fill rate, price improvement,
 └──────────────────────┘  latency p50 / p99 / p99.9
```

## Order book (step 1)

The book is "aggregated by price level", which is what ITCH gives us and what a
router needs: *how many shares are available at each price*.

- `orders_`: hash map `order id -> {side, price, shares}`. ITCH execute/cancel/delete
  messages only carry an order id, so we need this to find the order fast (O(1)).
- `bids_`: `std::map<price, Level>` sorted high → low, so `begin()` is the best bid.
- `asks_`: `std::map<price, Level>` sorted low → high, so `begin()` is the best ask.

Each `Level` stores total shares and number of orders at that price. When the last
order at a price leaves, the level is erased.

`std::map` is the simple, correct baseline. Step 5 adds two alternatives with the same
API (see below), and typed tests run every book test on all three.

## Venues (step 3)

There is only one real exchange in the data, so `MarketSimulator` **splits** the real
NASDAQ orders: each order id is hashed (splitmix64 with a fixed seed) to exactly one of
venues A/B/C, weighted 50/20/30. All later messages for that order (execute, cancel,
delete, replace) go to the same venue. As a result:

- each venue holds real prices and sizes, and real traders keep taking and cancelling them,
- the venues' best prices differ, because each holds different orders,
- A + B + C equals the real book, so no liquidity is counted twice (a unit test checks this).

Fees and latencies are assumed values chosen to create trade-offs (B is cheap but thin,
C is slow). They are not any real exchange's fees.

**Latency:** `submit()` puts our child order in a priority queue keyed by
`send_time + latency`. Before applying each ITCH message, the simulator executes every
order that has arrived by then, so a slow venue's order sees the book as it is *after*
the extra delay, including liquidity other traders took in the meantime.

**Simplification:** our fills don't remove shares from the venue books, because real
messages keep arriving for those same orders. That's accurate only for small orders that
are spaced out, which is how the experiments are designed.

## Routers and analytics (step 4)

A parent order (side, size, limit) is turned into child orders by one of three routers:

| Router | Idea |
|---|---|
| `single-venue` | Baseline. Send everything to the venue with the best top price, with the parent's limit. |
| `split` | Gather every level on every venue within the limit, sort by price, fill from the top. One child per venue, capped at the worst price chosen there. |
| `split+fees` | Same, but sort by price **plus fee** (buy) or **minus fee** (sell). |

Having three routers is an *ablation*: `split` vs `single-venue` shows what splitting
is worth, and `split+fees` vs `split` shows what fee awareness is worth.

`ExecutionStats` measures all-in cost against the **arrival mid** (the midpoint of the best
bid and ask across venues at decision time), including fees, in basis points.
`LatencyStats` reports nearest-rank percentiles.

The router stops reading a venue's book at the limit price, or once that venue alone could
fill the order (`OrderBook::for_each_level`). The first version copied every level with
`depth()`, and real books have thousands of far-away levels, so that change cut the median
decision time about 20x (169 µs to 7.4 µs for `split`), with identical routing results.

## Re-routing in waves (step 4b)

`OrderManager` works one parent order for one router:

1. **Wave 1:** the router splits the parent, and the child orders are sent.
2. **Wait for reports:** a child executes at `send + latency`, and its fill report reaches
   us at `execute + latency`, because the message travels back over the same link. A slow
   venue therefore costs twice: it executes late and reports late.
3. **Next wave:** once every report of the wave is back, if shares are still missing (and
   fewer than `max_waves` waves have gone out), the router is asked again with the
   *remaining* quantity and the *current* books. A quote that disappeared is no longer in
   the book, so the rest goes where liquidity really is. The parent's limit never changes.

With `max_waves = 1`, this is exactly the single-shot behaviour of step 4, so the tool runs
every strategy through the same `OrderManager` code.

## Faster order books (step 5)

| Book | Price levels | Order id → order |
|---|---|---|
| `OrderBook` | `std::map` (red-black tree) | `std::unordered_map` |
| `FlatOrderBook` | sorted `std::vector`, best price at the back | `std::unordered_map` |
| `FastOrderBook` | sorted `std::vector`, best price at the back | `FlatOrderMap` |
| `HybridOrderBook` | best 16 levels in a sorted `std::vector`, the rest in `std::map` | `FlatOrderMap` |

`FlatOrderMap` (`order_map.hpp`) is an open-addressing hash table with linear probing:
every entry lives in one array, kept at most half full, with Fibonacci hashing and
backward-shift deletion (no tombstones). `std::unordered_map` allocates a separate node per
order, so a lookup usually means a cache miss.

Changing one thing at a time shows where the time actually goes: `FlatOrderBook` changes
only the levels, and `FastOrderBook` also changes the hash map.

**Trade-off in the vector levels:** inserting a level shifts every *better-priced* level
toward the back. Near the best price that's a handful of elements, but real books are
thousands of levels deep (AAPL reached 4,227 bid levels), and about 6% of new levels shift
100 or more. Those rare shifts are the p99 tail. A tree costs O(log n) every time.

## Hybrid levels (step 5b)

`HybridLevels<N>` keeps the best N levels of a side in a `VectorLevels` ("hot") and every
worse level in a `std::map` ("cold"). Two rules keep it correct:

1. Every hot price is better than every cold price.
2. If cold isn't empty, hot holds exactly N levels.

- **Adding** a price at least as good as the worst hot level goes into hot. If hot now has
  N+1 levels, its worst level moves to cold. Anything worse goes into cold, unless hot still
  has room and cold is empty.
- **Removing** the last order at a hot level erases it, and the best cold level moves up to
  refill hot.

A vector shift now moves at most N levels, and deep levels cost O(log n), as in
`OrderBook`. Books are built from two pluggable parts (`OrderMap` and `Levels`), so each
variant is just a type alias. Tests run with N = 1, 2 and 4 to force constant movement across
the hot/cold boundary.

## Multithreaded pipeline (step 6)

`run_pipeline()` builds a `HybridOrderBook` for **every** stock in the full-day file:

```
 [gunzip] --chunks--> [parse] --events--> [books 1]   stocks with locate % 3 == 0
    ^                   |  \-----------> [books 2]   locate % 3 == 1
    \--free buffers----/   \----------> [books 3]   locate % 3 == 2
```

- **Stages:** `gunzip` fills 256 KB buffers from a pool of 16, and `parse` returns each empty
  buffer through a second queue, so nothing is allocated while running. `Framer` rebuilds
  ITCH frames that are split across two buffers. `parse` turns messages into 40-byte `Event`s,
  and each book thread owns its stocks outright, so no book is ever shared.
- **`SpscQueue`:** a ring buffer with one writer and one reader, and no locks:
  - the producer writes the slot, then publishes `tail` with `release`; the consumer reads
    `tail` with `acquire`, then reads the slot;
  - each side caches the other side's counter and re-reads it only when the queue looks
    full or empty;
  - the two counters sit on separate 128-byte cache lines, so the cores don't fight over one.
- **Layouts:** any stage can share a thread with the stage before it, so 1 to N threads run
  identical code. A per-stock running hash of the best bid and ask after every update
  (summed over stocks) must match across layouts.
- **Finding the bottleneck:** each thread records how long it waited on an empty input queue
  or a full output queue. The thread that almost never waits is the bottleneck.
- **False sharing fixed:** each `BookShard` is `alignas(128)`. The shards sit next to each
  other in a vector, and their counters change on every event.
