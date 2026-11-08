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
 │ Smart Order Router   │ ───▶ │ Venue A    │  real NASDAQ book
 │ (split parent order  │ ───▶ │ Venue B    │  simulated: lower fee, less liquidity
 │  by fee-adjusted     │ ───▶ │ Venue C    │  simulated: more liquidity, higher latency
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
