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

`std::map` is the simple, correct baseline. In step 5 we benchmark it against a
cache-friendly flat-array book and keep both, so the speedup is measured, not claimed.

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
