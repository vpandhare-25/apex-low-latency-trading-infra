# Apex — Low-Latency Multi-Venue Trading Engine

C++20 engine that rebuilds order books from real NASDAQ ITCH 5.0 data and routes orders
across three venues to get the best total price. Venue A is the real NASDAQ book;
venues B and C are **simulated** from it with different fees, latency and liquidity.

Status: step 1 of 7. The baseline order book is done and tested. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build and test
```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```
Requires CMake >= 3.20 and a C++20 compiler. GoogleTest downloads automatically.

Results will be added only from real benchmark runs, with hardware and dataset listed.
