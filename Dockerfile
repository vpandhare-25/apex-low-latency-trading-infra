# Reproducible Linux build of Apex: compiles with GCC, runs every unit test, and ships
# the tools in a small runtime image.
#
#   docker build -t apex .
#   docker run --rm -v "$PWD/data:/apex/data" apex apex_replay data/12302019.NASDAQ_ITCH50.gz AAPL
#
# The NASDAQ data file is not in the image (3.5 GB); mount it as shown above.

# ---- Build stage ---------------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make zlib1g-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /apex
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tools tools
COPY bench bench
COPY tests tests
# GoogleTest and Google Benchmark are downloaded by CMake (FetchContent) here.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---- Runtime stage -------------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends zlib1g \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /apex
COPY --from=build /apex/build/apex_replay /apex/build/apex_venues /apex/build/apex_route \
                  /apex/build/apex_extract /apex/build/apex_pipeline /apex/build/apex_bench \
                  /apex/build/apex_latency /apex/build/apex_depth /usr/local/bin/
CMD ["apex_replay"]
