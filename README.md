# Options Backtester

A C++20 foundation for downloading Alpaca market data, persisting it locally, and
building reproducible equity and options backtests. The current vertical slice
downloads paginated equity bars and upserts them into SQLite.

## Prerequisites

- CMake 3.22+
- A C++20 compiler
- Qt 6 Core, Widgets, and Charts development files
- libcurl development files
- SQLite 3 development files

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Download stock bars

Export credentials in your shell; do not commit them to a file:

```bash
export ALPACA_API_KEY="..."
export ALPACA_API_SECRET="..."
export ALPACA_DATA_FEED="iex"
export ALPACA_OPTION_FEED="indicative"

./build/debug/options-backtester download-bars \
  --symbols SPY,AAPL --start 2024-01-01 --end 2024-02-01 --timeframe 1Day
./build/debug/options-backtester count-bars --symbol SPY
```

The default database is `market-data.db`. Repeating a download updates matching
records rather than creating duplicates. Successfully downloaded date ranges are
cached, so repeating the same command makes no API request. Use `--refresh` to
force a new download. Each row records the provider, feed, timeframe, adjustment
mode, and retrieval time.

## Validate stored bars

Validation uses the exact symbol, feed, timeframe, adjustment mode, and optional
date range. It checks successful download coverage, timestamp ordering and
uniqueness, finite positive OHLC prices, OHLC relationships, and non-negative
volume and trade counts.

```bash
./build/debug/options-backtester validate-bars \
  --symbol SPY --start 2024-01-01 --end 2024-02-01
```

## Run the deterministic equity comparison

The initial strategy is a long-only moving-average crossover. A signal is formed
from completed closing prices and is executed at the next session's open. Both
the strategy and buy-and-hold use the same starting cash and whole shares. This
initial model has no commissions, slippage, dividends, or fractional shares.

```bash
./build/debug/options-backtester backtest-sma \
  --symbol SPY --start 2024-01-01 --end 2025-01-01 \
  --short-window 20 --long-window 50 --cash 10000
```

The selected period must contain more bars than the long SMA window. The command
prints final equity, total and annualized return, maximum drawdown, order count,
buy-and-hold results, and excess total return.

## Collect option data

The free Alpaca plan uses the `indicative` option feed. Set
`ALPACA_OPTION_FEED=opra` only when the account has OPRA access.

First download the contract universe for an explicit expiration range. Supplying
the range is important because Alpaca otherwise defaults to contracts expiring
before the upcoming weekend.

```bash
./build/debug/options-backtester download-option-contracts \
  --underlying SPY --expiration-from 2026-08-01 --expiration-to 2026-12-31

./build/debug/options-backtester list-option-contracts --underlying SPY --limit 50
```

Use OCC symbols printed by `list-option-contracts` to download historical bars
or collect current quotes:

```bash
./build/debug/options-backtester download-option-bars \
  --symbols SPY260821C00600000 \
  --start 2026-07-01 --end 2026-07-09 --timeframe 1Day

./build/debug/options-backtester collect-option-quotes \
  --symbols SPY260821C00600000
```

Collect a filtered daily chain snapshot containing the latest quote, latest
trade, implied volatility, and available Greeks:

```bash
./build/debug/options-backtester collect-option-snapshots \
  --underlying SPY --expiration-from 2026-08-01 --expiration-to 2026-09-30
```

Alpaca does not currently document a historical option-quotes endpoint. The
application therefore stores every latest quote collection and one point-in-time
snapshot per contract, market day, and feed. Running snapshot collection daily
builds the quote, IV, and Greeks history needed for future backtests without
using current values retroactively. Historical option bars are only available
from February 2024 onward.

## Run an intraday option snapshot server

The server command accepts multiple comma-separated underlyings and preserves
every poll in an append-only intraday observation table. It also updates the
daily latest-snapshot table used by the current daily options strategy.

```bash
./build/debug/options-backtester serve-option-snapshots \
  --underlyings SPY,AAPL \
  --expiration-from 2026-08-01 \
  --expiration-to 2026-09-30 \
  --interval-seconds 300 \
  --feed indicative \
  --db market-data.db
```

At startup the command queries Alpaca's US market clock. It exits immediately if
the regular market is not open. When started during a session, it uses Alpaca's
reported `next_close` and stops automatically at that time, including early-close
days. `Ctrl+C` and termination signals stop it cleanly. A failed collection for
one underlying is logged without stopping collections for the others.

The process is intentionally session-scoped: schedule it to launch shortly after
each market open rather than leaving it running overnight. Multiple processes may
share the same SQLite database, which uses WAL mode and a busy timeout, although
one process with several `--underlyings` is more API-efficient.

## Backtest the long-call strategy

This first options strategy selects a call nearest the requested delta within a
DTE range. It buys one contract at the observed ask, marks an open position at
the executable bid, and sells at the bid after the configured number of daily
snapshot sessions. Crossed, missing, non-positive, or excessively wide markets
are rejected. Open interest is copied into each snapshot when collected so later
contract updates cannot leak into older decisions.

The strategy requires at least two days of snapshots plus overlapping daily
underlying bars:

```bash
./build/debug/options-backtester backtest-long-call \
  --underlying SPY --start 2026-07-10 --end 2026-08-31 \
  --target-delta 0.50 --min-dte 30 --max-dte 45 \
  --hold-sessions 5 --max-spread 0.25 --min-open-interest 100 \
  --cash 10000
```

`--max-spread 0.25` means the bid/ask spread may be at most 25% of its midpoint.
The current model trades at most one contract at a time and does not yet include
commissions, assignment, exercise, or rolling.

## Qt results viewer

Launch the native Qt viewer without command-line options:

```bash
./build/debug/options-viewer
```

The viewer opens `market-data.db` automatically when launched from the repository
and remembers the last database selected with **Open Database**. Its symbol and
date selectors are populated from stored IEX daily equity bars. Switching symbols
or changing either date updates the chart, and each symbol's selected date range
is restored between sessions. Hold **Ctrl** while scrolling over the chart to move
the selected date window left or right along the available timeline. Every five
consecutive scroll events in one direction moves it by one step.

Choose **Momentum** from the chart's strategy dropdown to open its analysis
dialog. By default it evaluates a 30-day rolling window over the latest year
of stored data. For each trading date q, r is the first trading date on or after
q plus the configured number of calendar days. The default window is 30 days.
The skip window d controls how often a new comparison begins and defaults to one
day; for example, d=4 makes a Monday entry next eligible on Friday. If an eligible
date is not a trading day, the next stored trading day is used. The dialog reports
how often the close at r was greater than the close at q, along with win, loss,
tie, and total comparison counts. Both windows and the analysis date range are
configurable.

For d greater than one, momentum evaluates all d distinct calendar-phase starts.
It reports the average wins, losses, ties, comparisons, and win percentage for one
start phase rather than inflating the figures by aggregating all phases. This
reduces the effect of an unusually lucky or unlucky initial entry date.

Enable **Compare against an option strike** to replace q's stock close with a
synthetic option-strike threshold. Strikes are multiples of the configured dollar
width anchored at zero, so a $2.50 width produces $10.00, $12.50, $15.00, and so
on. Offset -1 selects the first strike strictly below q, +1 the first strictly
above q, and 0 the nearest strike; larger magnitudes move farther along the grid.
In parametric mode the strike-offset range is studied alongside x and d, while
the strike width remains fixed at the programmed value.

Enable **Use simulated spread pricing** to provide one-contract maximum-profit and
maximum-loss values for every studied strike offset. When negative and positive
offsets surround zero, offset 0 pricing is the midpoint of the nearest values on
each side. Wins add max profit, losses subtract max loss, and ties add zero. Total
profit and percentage profit appear in parametric results; percentage profit is
`((end value / allocation) - 1) × 100`, so break-even is 0%. Double-click a
row to open its simulated account-value chart. A non-parametric analysis opens its
chart immediately. Account curves and totals are averaged across skip-window start
phases in the same way as the momentum statistics. Parametric table headers use
**DTE**, **Skip**, and **Win Rate %**, and omit the average-ties column.

Enable **Run a parametric study** to evaluate inclusive ranges for both x and d.
The analysis dates remain fixed across every combination and are intentionally
not parametric. Every table column supports numeric sorting by clicking its header:
the first click sorts descending, the second ascending, and the third restores the
original unsorted study order. Selecting another header clears the previous sort.
Studies are limited to 10,000 parameter combinations per run. The ten combinations
with the largest number of comparisons are highlighted in green regardless of the
active ordering.
Single-value DTE, Skip, and Strike Offset fields are hidden while parametric mode
is active; their inclusive range controls take their place.

The **Load Data** tab downloads IEX daily bars for a new symbol into the currently
open database using `ALPACA_API_KEY` and `ALPACA_API_SECRET`. Its date selectors
default to Alpaca's maximum supported market-calendar range, 1970-01-01 through
today, and can be narrowed before loading. Previously covered ranges are read from
the local cache, so only missing ranges are requested from Alpaca.
