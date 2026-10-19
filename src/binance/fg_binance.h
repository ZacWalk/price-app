#ifndef FG_BINANCE_H
#define FG_BINANCE_H

#include <fg_data.h>

/* Application-owned acquisition; fg_ names retain the existing C contract. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fg_binance_market {
    FG_BINANCE_SPOT, FG_BINANCE_UM, FG_BINANCE_CM
} fg_binance_market;
typedef enum fg_binance_kind {
    FG_BINANCE_KLINES, FG_BINANCE_TRADES,
    FG_BINANCE_AGG_TRADES, FG_BINANCE_FUNDING_RATE, FG_BINANCE_METRICS
} fg_binance_kind;
typedef enum fg_binance_period {
    FG_BINANCE_DAILY, FG_BINANCE_MONTHLY
} fg_binance_period;

typedef struct fg_binance_file {
    fg_binance_market market;
    fg_binance_kind kind;
    fg_binance_period period;
    fg_str symbol;   /* [A-Z0-9][A-Z0-9_]{0,63}; no case conversion. */
    fg_str interval; /* Required for klines; must be empty for every other kind. */
    fg_str date;     /* Daily YYYY-MM-DD, monthly YYYY-MM, years 1970..9999. */
} fg_binance_file;

typedef struct fg_binance_limits {
    size_t max_archive_bytes; /* Zero: 256 MiB compressed. */
    size_t max_csv_bytes;     /* Zero: 512 MiB, both declared and actual size. */
    size_t max_rows;          /* Zero: 10 million data rows per file. */
    size_t max_archives;      /* Zero: 3660 files per window, checked up front. */
} fg_binance_limits;

typedef struct fg_binance_names {
    char zip_url[512], checksum_url[528];
    char zip_name[128], csv_name[128];
} fg_binance_names;

/* Pure validation/name construction. Intervals: 1s (spot only), 1m,3m,5m,
   15m,30m,1h,2h,4h,6h,8h,12h,1d,3d,1w,1mo and legacy 1M. The interval is
   preserved verbatim: 1mo is Binance's case-insensitive-filesystem spelling.
   URLs always use https://data.binance.vision/data/{spot|futures/um|futures/cm}.
   E.g. ETHUSDT-trades-2024-07-22.zip, ETHUSDT-1m-2024-07.zip;
   the sidecar is the ZIP URL followed by .CHECKSUM, not a CSV checksum.
   Download-only kinds use the exact provider spellings aggTrades, fundingRate
   and metrics in both the directory and filename. Valid name construction is
   not an availability guarantee; missing market/period archives return 404. */
fg_status fg_binance_build_names(const fg_binance_file *, fg_binance_names *, fg_error *);

/* Fetch ZIP, then mandatory .CHECKSUM. OpenSSL EVP SHA256 verifies compressed
   bytes BEFORE libarchive sees them. Only one regular, unencrypted ZIP member
   with the exact expected basename is accepted. No filesystem extraction.
   Both the streaming and central-directory ZIP views are checked, including
   CRCs. Stored/deflated ZIPs supported; no outer compression filters.
   csv is an initialized owning buffer; unchanged on error. This function
   validates the archive, NOT the CSV schema (use parse_csv or fetch for that).
   The client's max_response_bytes bounds each response DURING transport;
   max_archive_bytes is an additional check after fg_get. Configure both for
   stricter network limits. Checksum text is additionally limited to 4096 bytes.
   ZIP HTTP 404 => FG_NOT_FOUND. Checksum HTTP 404 => FG_CHECKSUM (http_status
   remains 404), so optional missing-archive handling cannot bypass verification.
   Other HTTP failures => FG_HTTP, digest/sidecar failures => FG_CHECKSUM,
   unsafe/corrupt ZIP => FG_PARSE, limits => FG_LIMIT.
   Transport failures propagate, never become "not available". No redirects,
   cookies, retries, checksum bypass, implicit environment or disk writes.
   Injected transports must also disable redirects and cookies. */
fg_status fg_binance_download(fg_client *, const fg_binance_file *,
    const fg_binance_limits *, fg_buffer *csv, fg_error *);

typedef struct fg_binance_kline {
    int64_t open_time_us, close_time_us;
    fg_str open, high, low, close, volume;
    fg_str asset_volume, taker_buy_volume, taker_buy_asset_volume, ignore;
    uint64_t trade_count;
} fg_binance_kline;

typedef struct fg_binance_trade {
    uint64_t id;
    fg_str price, quantity, asset_quantity;
    int64_t time_us;
    int buyer_maker;
    int has_best_match, best_match;
} fg_binance_trade;

typedef struct fg_binance_record {
    fg_binance_kind kind;
    union {
        fg_binance_kline kline;
        fg_binance_trade trade;
    } value;
    const fg_str *fields; /* Original CSV-decoded lexemes, no header. */
    size_t field_count, row_index; /* Zero-based data row index. */
} fg_binance_record;

/* Everything passed to the callback is borrowed until return. Copy to retain.
   The callback's status/error propagate unchanged; earlier callbacks are not
   rolled back on later malformed CSV, callback failure or a later file error.
   No callback is invoked until checksum AND all ZIP validation has completed.

   Typed records support KLINES and TRADES only. AGG_TRADES, FUNDING_RATE and
   METRICS are download-only: use fg_binance_download to obtain verified raw CSV.
   parse_csv/fetch/fetch_window reject these kinds with FG_INVALID before any
   callback or network request; they never reinterpret them as trades.

   All klines have 12 columns. Spot trades have 7 (including isBestMatch);
   UM/CM trades have 6, matching the legacy crypto-app USD-M input, and never
   fabricate a seventh field. Optional headers must match the known column
   order/schema; snake_case and provider camelCase spellings are recognized.
   Headerless files are valid; empty/header-only files are not.

   All timestamps are exact Unix int64 microseconds. Spot archive dates from
   2025-01 onward contain microseconds; earlier spot and ALL futures archives
   contain milliseconds, multiplied by 1000 with overflow checking. Units are
   selected by MARKET + ARCHIVE DATE, never guessed from magnitude or floats.
   Trade/open times must belong to the named archive period; close times must
   be >= open time (they may extend beyond the period for long intervals).
   IDs/counts are exact uint64; bool accepts only true/false (case insensitive).
   Decimal fields remain exact, nonnegative JSON-number lexemes (<=256 bytes),
   validated with fg_data. No implicit double approximation. Consumers wanting
   an approximation can explicitly use fg_json_new_number + fg_json_double.
   For spot/UM, asset_volume/asset_quantity are QUOTE amounts. For CM they are
   BASE amounts; CM volume/quantity and taker_buy_volume are contract counts,
   retained as decimal text, not reinterpreted as base-asset quantity. */
typedef fg_status (*fg_binance_callback)(void *, const fg_binance_file *,
    const fg_binance_record *, fg_error *);
fg_status fg_binance_parse_csv(const fg_binance_file *, fg_str csv,
    const fg_binance_limits *, fg_binance_callback, void *, fg_error *);
fg_status fg_binance_fetch(fg_client *, const fg_binance_file *,
    const fg_binance_limits *, fg_binance_callback, void *, fg_error *);

typedef struct fg_binance_window {
    fg_binance_market market;
    fg_binance_kind kind;
    fg_binance_period period;
    fg_str symbol, interval;
    fg_str start_date, end_date; /* Both REQUIRED, inclusive YYYY-MM-DD. */
} fg_binance_window;

/* Ascending explicit dates, no wall clock/default window. Daily selects each
   day; monthly selects the WHOLE months touched by the inclusive date window,
   matching dd.ps1 (records are not clipped to start/end). Stops on the first
   error INCLUDING 404; a caller may fetch individual files to skip missing
   archives explicitly. Memory is released between files. */
fg_status fg_binance_fetch_window(fg_client *, const fg_binance_window *,
    const fg_binance_limits *, fg_binance_callback, void *, fg_error *);

#ifdef __cplusplus
}
#endif
#endif
