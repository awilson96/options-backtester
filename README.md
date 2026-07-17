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
  --symbols SPY,AAPL --start 2024-01-01 --end 2024-02-01
./build/debug/options-backtester count-bars --symbol SPY
```

Stock downloads default to Alpaca's finest supported bar resolution, `1Min`.
Pass a different `--timeframe` explicitly only when coarser aggregates are wanted.
Minute history contains substantially more rows than daily history, so broad date
ranges take longer to download, store, analyze, and draw.

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
from completed bar closing prices and is executed at the next bar's open. Both
the strategy and buy-and-hold use the same starting cash and whole shares. This
initial model has no commissions, slippage, dividends, or fractional shares.

```bash
./build/debug/options-backtester backtest-sma \
  --symbol SPY --start 2024-01-01 --end 2025-01-01 \
  --short-window 20 --long-window 50 --cash 10000
```

The SMA window values count bars, so the defaults are 20 and 50 minutes when using
the default `1Min` stock data. The selected period must contain more bars than the
long SMA window. The command
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

The strategy requires at least two days of snapshots plus overlapping one-minute
underlying bars. The first and final underlying bars in each session supply that
session's benchmark open and close:

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
date selectors are populated from stored IEX one-minute equity bars. Switching
symbols or changing either date updates the chart, and each symbol's selected date range
is restored between sessions. Hold **Ctrl** while scrolling over the chart to move
the selected date window left or right along the available timeline. Every five
consecutive scroll events in one direction moves it by one step.
Hovering over the underlying chart snaps a dashed vertical guide to the nearest
displayed candle and shows its timestamp, OHLC, and volume in a box to the
left of the guide. The redundant horizontal-axis time labels are hidden because the
hover box provides the precise timestamp. Timestamps use the `America/New_York` time zone, including the
EST/EDT daylight-saving transition. Only regular market hours from 9:30 a.m. up
to 4:00 p.m. Eastern are plotted. Closed and missing periods are removed from the
horizontal scale, so one session's final candle is followed immediately by the
next session's 9:30 a.m. candle. Aggregated observations are drawn with high/low
whiskers, a green body when close is above open, and a red body when close is
below open. A single-day view defaults to **1 Min** candles; any multi-day view
defaults to **1 Day** candles. The **Candles** dropdown at the top right can
override that default with **1 Day**, **1 Hour**, **30 Min**, **15 Min**, **5 Min**,
or **1 Min**. Intraday buckets are anchored at the 9:30 a.m. open, so the final
1-hour candle contains the last 30 minutes of the regular session. The selected
symbol and candle resolution persist between application runs. Start and end dates
are persisted separately for each symbol and restored within that symbol's available
stored-data range.

Click and drag horizontally across the underlying chart to display a translucent
selection and zoom when the mouse is released. A selection spanning multiple dates
updates and persists the page's start/end dates before rebuilding the chart. A
selection within one trading day zooms the existing chart directly so intraday
ranges are retained. Its vertical price axis is recalculated from the selected
candles so their minimum and maximum fill the chart height with a small margin.
Drags narrower than eight pixels are treated as ordinary clicks, and right-clicking
resets both axes to the complete chart range.

### Intraday distribution study foundation

The replacement intraday study calculation always uses one-minute regular-session
candles. A session is accepted only when it contains one unique candle for every
minute from 9:30 a.m. through 3:59 p.m. Eastern; incomplete, short, invalid, and
duplicate-minute sessions are excluded and counted.

Each accepted session stores its 390 one-minute candles, using `(open + close) / 2`
as the candle-count-weighted analysis price. The calculation independently derives
the lowest and highest **50%**, **40%**, **30%**, **20%**, and **10%** of candles for
that session. Quantile-boundary ties are retained even when this makes a membership
slightly larger than its target. Each membership preserves both its individual
minute offsets and its piecewise contiguous time ranges. Actual session-low and
session-high occurrence minutes are recorded separately from the quantile sets.

The accepted sessions are aggregated independently into Monday-through-Friday
records. Every weekday record contains 390-bin, one-minute-resolution histograms for
actual low occurrences, actual high occurrences, and each downside and upside
quantile membership. Histogram bins store occurrence counts, and the aggregation
also records total observations, the peak count, and every tied peak minute. Tied
daily lows and highs increment every minute where that extreme occurred.

Choose **Intraday Distribution** from the strategy dropdown to open its dedicated
strategy window. The Monday-through-Friday tabs live inside that window rather than
beside the application's top-level **Underlying Price** tab. Each weekday page shows
a selectable 390-bin histogram above its session ledger. The histogram selector can
display actual low/high occurrences or any 50%, 40%, 30%, 20%, or 10% downside or
upside membership distribution. Selecting a histogram on any weekday page updates
the selector and histogram on every other weekday page, making direct day-to-day
comparisons possible without repeating the selection five times.

The ledger reports each session's daily OHLC, median, actual low/high occurrence
times, and selected quantile membership size and segment count. Hovering a ledger
row overlays that session's contributing minutes in orange on the currently selected
aggregate histogram. The weekly ranking summary will be layered on this interface
in a subsequent implementation step.

Double-click a ledger row to construct its one-day detail chart on demand. No day
chart or candlestick graphics are created during the study calculation or weekday
page setup. The detail view draws all 390 one-minute candlesticks, marks every tied
session-low and session-high occurrence, and displays solid downside and dashed
upside price-threshold lines for the 50%, 40%, 30%, 20%, and 10% memberships.

Hovering one of the colored percentage labels shades only that membership's
piecewise qualifying minutes: downside shading extends from its threshold toward
the bottom of the chart, while upside shading extends from its threshold toward the
top. The levels use yellow, green, blue, purple, and pink respectively from 50%
through 10%. Closing the detail dialog destroys its on-demand chart resources.

The strategy's first internal tab is a **Summary** page. Every week containing at
least one accepted session is included, including holiday and partial weeks. For
lows, rank 1 is the lowest low among the weekdays present that week. For highs, rank
1 is the highest high among the weekdays present. Ranks extend through the number of
available sessions, so a four-session holiday week uses ranks 1 through 4. Each
weekday's averages and win rates use only weeks in which that weekday participated.
The page reports participation counts, average ranks, weekly-low and weekly-high win
counts and rates, the study minimum and maximum, and the number of partial weeks.
Price ties share their average rank and every tied winner receives a win.

Completed calculation results are cached in memory by database, ticker, and date
range. The cache contains session records, quantile memberships, weekday histogram
counts, and weekly ranking summaries, so changing tabs or histogram percentages does
not reread or reprocess one-minute bars. It is bounded to six study ranges and is
cleared when the database changes or new bars are stored. Day-chart widgets,
candlestick drawing geometry, and 10–50% hover highlighting are deliberately never
cached; those resources remain on demand and are destroyed when their dialog closes.

Choose **Momentum** from the chart's strategy dropdown to open its analysis
dialog. By default it evaluates a 30-day rolling window over the latest year
of stored data. For each bar q, r is the first stored bar at or after q's exact
timestamp plus the configured number of calendar days. The default window is 30
days.
The skip window d controls how often a new comparison begins and defaults to one
day; for example, d=4 makes a Monday entry next eligible on Friday. If an eligible
date is not a trading day, the next stored trading day is used. The dialog reports
how often the one-minute analysis price at r was greater than at q, along with win, loss,
tie, and total comparison counts. Both windows and the analysis date range are
configurable.

Select **Save Study…** in the Momentum dialog to store a named preset. Saved
studies appear in a dropdown above the chart on the **Underlying Price** page.
Choose a study and select **Load** to select its associated ticker and open Momentum with its
single or parametric ranges, analysis dates, strike settings, simulated pricing,
allocation, drop rate, and slippage restored. Loading only fills the dialog; it
does not run the analysis until **Run Analysis** or **Run Parametric Study** is
selected. Select **Delete** to remove the chosen preset after confirming the
action. The ticker is editable from a dropdown inside the study dialog. Changing
it preserves the analysis period where possible, clamps the dates to the new
symbol's stored history, and leaves the other strategy parameters in place. Saving
with the loaded study's existing name updates it; saving under a different name
creates a new preset so the original can continue serving as a template. Presets
with the same name but different symbols are also stored separately; replacement
is only offered when both the name and symbol match. Presets use a versioned
field-based format, so parameters introduced by
future releases can use their defaults when an older study is loaded and can then
be adjusted before running or saving again.

Selecting **Save Study…** stores the study parameters and, when those exact
settings have just been run, the completed single result or parametric table,
summary statistics, and underlying aggregated bars. Single-result studies retain their
chart data, while parametric studies save summaries only. A saved study restores that
result when explicitly loaded and does not open a profit-chart popup until a table
row is selected. Pressing **Run Analysis** always performs a fresh calculation;
there is no automatic result-cache lookup or reuse. If the displayed parameters
have not been run, only the study template is saved.

Momentum defaults to VWAP as the analysis price, falling back to close only when
VWAP is unavailable. The Momentum dialog can aggregate regular-session data into
**1 Day**, **1 Hour**, **30 Min**, **15 Min**, **5 Min**, or **1 Min** bars and analyze
each bar's **Open**, **Close**, **High**, **Low**, **Average**, or **VWAP**. **Average**
is the arithmetic mean of that aggregated candle's open and close. Multi-day studies
default to daily data and single-day studies default to one-minute data. Intraday
buckets are anchored at 9:30 a.m. Eastern; VWAP is volume-weighted when bars are
aggregated. These selections are saved with study presets and included in result
signatures. **Drop rate** is a fixed, non-parametric percentage
of otherwise eligible entries treated as unfillable. Dropped entries are selected
using five reproducible pseudo-random symbol/timestamp scenarios per strategy row. Table
statistics use the scenario with median total profit. Profit charts show the high
scenario in green, low in red, median in black, and a zero-drop baseline as a blue
dotted line. Hovering over a profit chart displays a vertical date guide and a
pastel-grey summary containing the date plus the no-drop, high, low, and median
account values at that position. The underlying asset's selected aggregated analysis
price is drawn in very light grey against a separate price axis on the right. It shares the
chart's date axis but cannot change the account-value scale on the left. No
additional scenario columns are added to the parametric table.

For d greater than one, momentum evaluates all d distinct calendar-phase starts.
It reports the average wins, losses, ties, comparisons, and win percentage for one
start phase rather than inflating the figures by aggregating all phases. This
reduces the effect of an unusually lucky or unlucky initial entry date.

Enable **Compare against an option strike** to replace q's stock close with a
synthetic option-spread threshold. **Resolution** is the spacing between offered
lower-leg strikes, while **Strike Width** is the distance between the lower and
upper legs of each spread. Both are fixed, non-parametric settings. With $1
resolution and $5 width, the available spreads include 490/495, 491/496, and
492/497. Offset -1 selects one resolution step below the current-price boundary,
+1 selects one step above it, and larger magnitudes move farther along the
resolution grid; offset 0 uses the nearest grid price. The selected grid price is
the lower spread leg, and the comparison threshold is `lower leg + strike width`.
For example, price $756.56 with $1 resolution, $5 width, and offset -2 produces
755/760 and a $760 comparison price. Price $451.32 with $2.50 resolution, $5
width, and offset +2 produces 455/460 and a $460 comparison price. The ledger and
hover shading use this same upper spread boundary. In parametric mode only the
strike-offset range is swept alongside x and d.

Enable **Use simulated spread pricing** to provide one-contract maximum-profit and
maximum-loss values for every studied strike offset. When negative and positive
offsets surround zero, offset 0 pricing is the midpoint of the nearest values on
each side. Wins add max profit, losses subtract max loss, and ties add zero. Total
profit and percentage profit appear in parametric results; percentage profit is
`((end value / allocation) - 1) × 100`, so break-even is 0%. Double-click a
row to open its simulated account-value chart. A non-parametric analysis opens its
chart immediately. Chart titles identify `DTE`, `skip`, `res`, `strike_width`, and
the configured `strike_offset`. Account curves and totals are averaged across skip-window start
phases in the same way as the momentum statistics. Parametric table headers use
**DTE**, **Skip**, and **Win Rate %**, omit the average-ties and Rank columns, and
use the table's built-in row numbers for the current rank.

Profit curves and trade ledgers are generated on demand rather than during the
parametric sweep. Summary rows calculate total profit directly without constructing
or retaining per-date curve points.
Double-clicking a simulated-pricing result row reconstructs only that row's
deterministic median drop scenario on the background worker, then opens its
four-line profit chart with an executed-trades table underneath. The ledger lists
start and end dates, underlying start and end prices, comparison price, ITM/OTM
(or ATM for an exact tie), skip-start phase, realized one-contract profit or loss,
and money currently at risk. This is the concurrent max-loss capital, including
applicable slippage, reserved immediately after that trade opens within its
skip-start phase. Positions that have already expired are released first, and the
value cannot exceed that phase's current account balance. Only funded executions are included; dropped opportunities and trades
skipped for insufficient capital are excluded. Because skip values above one are
averaged across multiple start schedules, the phase column identifies which
schedule produced each execution. The generated ledger is reused for later opens
of that row during the current session.

Hovering over an ITM or OTM ledger row focuses its holding period on the chart.
The four profit curves fade to light variants of their original colors, the
underlying-price line becomes black, vertical guides mark the start and end dates,
and a horizontal boundary marks that trade's computed comparison strike. Within
the start-to-end interval, the area above the strike is shaded green and the area
below it red. The boundary uses the exact strike grid and offset for the selected
strategy row, including offsets such as -2 or +2. Moving away from the row restores
the normal chart styling. ATM rows intentionally have no hover treatment yet.

Simulated pricing also supports fixed slippage on neither side, the buy only, the
sell only, or both buy and sell. Buy and sell slippage have independent inputs and
are entered as dollars per share: `$0.04` means four cents per share, or `$4` per
100-share option contract. Each enabled side reduces max profit and increases max
loss by its configured amount; for example, one-sided `$0.04` slippage changes
`$200/$300` to `$196/$304`. The same slippage configuration is applied to every
row of a parametric study.

The pseudo-backtest reserves adjusted max-loss capital for every open contract.
Overlapping entries that cannot be funded by the user's current account value are
skipped, and only funded trades affect the table and account-value chart. **Capital
Needed** reports the minimum starting capital that would have executed the complete
historical schedule, including overlapping risk and the effect of earlier realized
losses. For analyses averaged across skip phases, this is the maximum requirement
among those phases.

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

Momentum calculations run on a background worker so long parametric studies do
not block the desktop interface or cause operating-system “not responding”
warnings. Independent parameter combinations are distributed through a thread-safe
work queue. On systems with four or more reported logical
cores, roughly one quarter (and at least two) are reserved for the Qt interface,
operating system, and memory-bandwidth headroom; smaller systems reserve one core
when possible. The remaining cores run analysis workers, avoiding
the event-loop starvation that can otherwise trigger false “not responding”
warnings under sustained load. Results are written back in their original
deterministic order. While a study is running, the progress bar reports completed,
total, and remaining parameter combinations, and calculation completion returns
directly to the results table without writing a result file. On reruns, the prior
result is released on the analysis worker before new result storage is allocated.
The completed result is moved, rather than copied, back to the interface, and table
display uses a lazy model backed directly by those rows. Only visible cell text is
formatted; loading no longer creates one widget item per table cell, and sorting
reorders lightweight row indices rather than table objects.
Selecting **Save
Study…** writes the result attached to that study on a background worker with
saved-row and remaining-row progress. Each summary strategy row is stored as an
independent block so
saved parametric results can be decoded concurrently using the same reserved-core
policy as calculation. Older result files remain readable; their obsolete embedded
curves are skipped during loading and removed the next time the study is saved.
Result-write failures are
reported without discarding the completed analysis or saved parameters. Parameter,
save, and run controls are temporarily locked during their respective background
operations so displayed results always correspond to the captured settings.

The Momentum dialog opens at a screen-aware size, can be maximized or expanded
without an application-imposed maximum width, and automatically grows when
parametric or simulated-pricing controls need additional room. Result columns keep
readable widths and scroll horizontally when necessary, while large collections
of strike-pricing inputs scroll vertically instead of compressing other fields.

The **Load Data** tab downloads IEX one-minute bars for a new symbol into the currently
open database using `ALPACA_API_KEY` and `ALPACA_API_SECRET`. Its date selectors
default to the latest year to keep one-minute downloads manageable, but the start
can be moved as far back as 1970-01-01. Previously covered ranges are read from
the local cache, so only missing ranges are requested from Alpaca.
