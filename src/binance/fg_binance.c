#include "fg_binance.h"
#include <fg_source_utils.h>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINANCE_ARCHIVE_LIMIT ((size_t)256 * 1024 * 1024)
#define BINANCE_CSV_LIMIT ((size_t)512 * 1024 * 1024)
#define BINANCE_ROW_LIMIT ((size_t)10000000)
#define BINANCE_FILE_LIMIT ((size_t)3660)

typedef struct binance_period {
    int64_t begin_us, end_us;
    int microseconds;
} binance_period;

static size_t archive_limit(const fg_binance_limits *l)
{ return l && l->max_archive_bytes ? l->max_archive_bytes : BINANCE_ARCHIVE_LIMIT; }
static size_t csv_limit(const fg_binance_limits *l)
{ return l && l->max_csv_bytes ? l->max_csv_bytes : BINANCE_CSV_LIMIT; }
static size_t row_limit(const fg_binance_limits *l)
{ return l && l->max_rows ? l->max_rows : BINANCE_ROW_LIMIT; }
static size_t file_limit(const fg_binance_limits *l)
{ return l && l->max_archives ? l->max_archives : BINANCE_FILE_LIMIT; }

static int valid_symbol(fg_str s)
{
    size_t i;
    if (!s.data || !s.len || s.len > 64) return 0;
    for (i = 0; i < s.len; ++i) {
        char c = s.data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c != '_' || !i) return 0;
    }
    return 1;
}

static int valid_interval(fg_str s, fg_binance_market market)
{
    static const char *const intervals[] = {
        "1m","3m","5m","15m","30m","1h","2h","4h","6h","8h","12h","1d","3d","1w","1mo","1M"
    };
    size_t i;
    if (!s.data) return 0;
    if (fg_str_equal(s, FG_STR("1s"))) return market == FG_BINANCE_SPOT;
    for (i = 0; i < sizeof(intervals)/sizeof(*intervals); ++i)
        if (fg_str_equal(s, FG_STR(intervals[i]))) return 1;
    return 0;
}

static fg_status period_info(const fg_binance_file *f, binance_period *p, fg_error *e)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    char date[11];
    int64_t begin;
    int year, month, count = 1;
    if (!f || !p || f->market < FG_BINANCE_SPOT || f->market > FG_BINANCE_CM ||
        f->kind < FG_BINANCE_KLINES || f->kind > FG_BINANCE_METRICS ||
        f->period < FG_BINANCE_DAILY || f->period > FG_BINANCE_MONTHLY ||
        !valid_symbol(f->symbol) || (!f->interval.data && f->interval.len) ||
        (f->kind != FG_BINANCE_KLINES ? f->interval.len != 0 : !valid_interval(f->interval, f->market)) ||
        !f->date.data || f->date.len != (f->period == FG_BINANCE_DAILY ? 10u : 7u))
        return fg_fail(e, FG_INVALID, "Invalid Binance market, kind, symbol, interval or date");
    memcpy(date, f->date.data, f->date.len);
    if (f->period == FG_BINANCE_MONTHLY) memcpy(date + 7, "-01", 3);
    date[10] = 0;
    if (fg_s_date(FG_STR(date), &begin, NULL) != FG_OK || begin < 0)
        return fg_fail(e, FG_INVALID, "Binance date must be a real calendar date in 1970..9999");
    year = (date[0]-'0')*1000 + (date[1]-'0')*100 + (date[2]-'0')*10 + date[3]-'0';
    month = (date[5]-'0')*10 + date[6]-'0';
    if (f->period == FG_BINANCE_MONTHLY)
        count = days[month-1] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0));
    p->begin_us = begin * INT64_C(1000000);
    p->end_us = (begin + (int64_t)count * 86400) * INT64_C(1000000);
    p->microseconds = f->market == FG_BINANCE_SPOT && year >= 2025;
    return FG_OK;
}

fg_status fg_binance_build_names(const fg_binance_file *f, fg_binance_names *out, fg_error *e)
{
    static const char *const kinds[] = {"klines", "trades", "aggTrades", "fundingRate", "metrics"};
    fg_binance_names n = {0};
    binance_period p;
    const char *market, *period, *kind;
    char stem[120], prefix[320];
    fg_status s;
    if (!out) return fg_fail(e, FG_INVALID, "Missing Binance names output");
    s = period_info(f, &p, e);
    if (s != FG_OK) return s;
    market = f->market == FG_BINANCE_SPOT ? "spot" : f->market == FG_BINANCE_UM ? "futures/um" : "futures/cm";
    period = f->period == FG_BINANCE_DAILY ? "daily" : "monthly";
    kind = kinds[f->kind];
    if (f->kind == FG_BINANCE_KLINES) {
        (void)snprintf(stem, sizeof(stem), "%.*s-%.*s-%.*s", (int)f->symbol.len, f->symbol.data,
            (int)f->interval.len, f->interval.data, (int)f->date.len, f->date.data);
        (void)snprintf(prefix, sizeof(prefix), "https://data.binance.vision/data/%s/%s/%s/%.*s/%.*s",
            market, period, kind, (int)f->symbol.len, f->symbol.data, (int)f->interval.len, f->interval.data);
    } else {
        (void)snprintf(stem, sizeof(stem), "%.*s-%s-%.*s", (int)f->symbol.len, f->symbol.data,
            kind, (int)f->date.len, f->date.data);
        (void)snprintf(prefix, sizeof(prefix), "https://data.binance.vision/data/%s/%s/%s/%.*s",
            market, period, kind, (int)f->symbol.len, f->symbol.data);
    }
    (void)snprintf(n.zip_name, sizeof(n.zip_name), "%s.zip", stem);
    (void)snprintf(n.csv_name, sizeof(n.csv_name), "%s.csv", stem);
    (void)snprintf(n.zip_url, sizeof(n.zip_url), "%s/%s", prefix, n.zip_name);
    (void)snprintf(n.checksum_url, sizeof(n.checksum_url), "%s.CHECKSUM", n.zip_url);
    *out = n;
    return FG_OK;
}

static fg_status get_archive(fg_client *client, const char *url, fg_response *out, fg_error *e)
{
    fg_status s = fg_get(client, FG_STR(url), NULL, 0, out, e);
    int http = out->status;
    if (s == FG_HTTP && http == 404) s = FG_NOT_FOUND;
    if (s == FG_OK && http != 200) s = FG_HTTP;
    if (s != FG_OK) {
        fg_fail(e, s, s == FG_NOT_FOUND ? "Binance archive or checksum not found" : "Binance request failed");
        if (e) e->http_status = http;
    }
    return s;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static fg_status verify_digest(fg_str zip, fg_str checksum, const char *name, fg_error *e)
{
    unsigned char expected[32], actual[EVP_MAX_MD_SIZE];
    unsigned int actual_len = 0;
    size_t i, length = strlen(name);
    EVP_MD_CTX *ctx;
    int ok;
    if (checksum.len && checksum.data[checksum.len-1] == '\n') {
        --checksum.len;
        if (checksum.len && checksum.data[checksum.len-1] == '\r') --checksum.len;
    }
    if (checksum.len != 66 + length || checksum.data[64] != ' ' ||
        (checksum.data[65] != ' ' && checksum.data[65] != '*') ||
        memcmp(checksum.data + 66, name, length) != 0)
        return fg_fail(e, FG_CHECKSUM, "Invalid Binance checksum filename or format");
    for (i = 0; i < 32; ++i) {
        int a = hex_digit((unsigned char)checksum.data[2*i]);
        int b = hex_digit((unsigned char)checksum.data[2*i+1]);
        if (a < 0 || b < 0) return fg_fail(e, FG_CHECKSUM, "Invalid Binance SHA256 digest");
        expected[i] = (unsigned char)((a << 4) | b);
    }
    ctx = EVP_MD_CTX_new();
    if (!ctx) return fg_fail(e, FG_NOMEM, "Cannot allocate SHA256 context");
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, zip.data, zip.len) == 1 &&
         EVP_DigestFinal_ex(ctx, actual, &actual_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || actual_len != sizeof(expected))
        return fg_fail(e, FG_IO, "OpenSSL SHA256 operation failed");
    if (CRYPTO_memcmp(expected, actual, sizeof(expected)) != 0)
        return fg_fail(e, FG_CHECKSUM, "Binance SHA256 mismatch");
    return FG_OK;
}

static uint32_t zip_u16(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t zip_u32(const unsigned char *p)
{ return zip_u16(p) | (zip_u16(p + 2) << 16); }
static uint64_t zip_u64(const unsigned char *p)
{ return zip_u32(p) | ((uint64_t)zip_u32(p + 4) << 32); }

static int zip_directory(fg_str zip, size_t end, size_t *offset, size_t *size, size_t *directory_end)
{
    const unsigned char *data = (const unsigned char *)zip.data, *p = data + end;
    uint64_t off = zip_u32(p + 16), len = zip_u32(p + 12);
    uint32_t count = zip_u16(p + 10);
    *directory_end = end;
    if (zip_u16(p + 4) || zip_u16(p + 6) || zip_u16(p + 8) != count) return 0;
    if (end >= 20 && memcmp(data + end - 20, "PK\006\007", 4) == 0) {
        const unsigned char *locator = data + end - 20, *z;
        uint64_t position = zip_u64(locator + 8);
        if (zip_u32(locator + 4) || zip_u32(locator + 16) != 1 ||
            position > end - 20 || end - 20 - position < 56) return 0;
        z = data + (size_t)position;
        if (memcmp(z, "PK\006\006", 4) != 0 || zip_u64(z + 4) != end - 20 - position - 12 ||
            zip_u32(z + 16) || zip_u32(z + 20) || zip_u64(z + 24) != 1 ||
            zip_u64(z + 32) != 1 || (count != 1 && count != 65535)) return 0;
        if ((off != UINT32_MAX && off != zip_u64(z + 48)) ||
            (len != UINT32_MAX && len != zip_u64(z + 40))) return 0;
        off = zip_u64(z + 48); len = zip_u64(z + 40);
        *directory_end = (size_t)position;
    } else if (count != 1) return 0;
    if (off > *directory_end || len > *directory_end - off) return 0;
    *offset = (size_t)off; *size = (size_t)len;
    return 1;
}

/* libarchive's ZIP reader normalizes some nonregular Unix modes to regular
   files and tolerates truncated streaming directories. Check the original
   metadata/envelope too; libarchive still owns decompression and CRC checking. */
static int zip_envelope(fg_str zip, const char *name)
{
    size_t i, first;
    const unsigned char *local = (const unsigned char *)zip.data;
    size_t name_len = strlen(name);
    if (zip.len < 22 || memcmp(zip.data, "PK\003\004", 4) != 0) return 0;
    first = zip.len > 65557 ? zip.len - 65557 : 0;
    i = zip.len - 22;
    for (;;) {
        if (memcmp(zip.data + i, "PK\005\006", 4) == 0) {
            const unsigned char *p = (const unsigned char *)zip.data + i;
            size_t comment = zip_u16(p + 20);
            if (zip.len - i == 22 + comment) {
                size_t offset, size, directory_end;
                const unsigned char *central;
                uint32_t mode, flags, method;
                if (!zip_directory(zip, i, &offset, &size, &directory_end) ||
                    size < 46 || size != directory_end - offset || offset < 30 ||
                    memcmp(zip.data + offset, "PK\001\002", 4) != 0) return 0;
                central = local + offset;
                if (zip_u16(central + 28) != name_len ||
                    size != 46 + name_len + zip_u16(central + 30) + zip_u16(central + 32) ||
                    memcmp(central + 46, name, name_len) != 0 ||
                    zip_u16(central + 34) != 0 || zip_u32(central + 42) != 0 ||
                    zip_u16(local + 26) != name_len || offset - 30 < name_len ||
                    zip_u16(local + 28) > offset - 30 - name_len ||
                    memcmp(local + 30, name, name_len) != 0) return 0;
                flags = zip_u16(central + 8);
                method = zip_u16(central + 10);
                if (flags != zip_u16(local + 6) || (flags & 0x2041u) ||
                    method != zip_u16(local + 8) || (method != 0 && method != 8)) return 0;
                mode = (zip_u32(central + 38) >> 16) & 0170000u;
                if ((central[5] == 3 || central[5] == 19) && mode && mode != AE_IFREG) return 0;
                if (central[38] & 0x10u) return 0;
                return 1;
            }
        }
        if (i == first) break;
        --i;
    }
    return 0;
}

static fg_status unzip_pass(fg_str zip, const char *name, size_t limit, int seekable,
                            fg_buffer *csv, fg_error *e)
{
    struct archive *a = archive_read_new();
    struct archive_entry *entry = NULL;
    fg_status s = FG_OK;
    int rc, entries = 0;
    size_t total = 0;
    char block[32768];
    if (!a) return fg_fail(e, FG_NOMEM, "Cannot allocate ZIP reader");
    rc = archive_read_support_filter_none(a);
    if (rc == ARCHIVE_OK) rc = seekable ? archive_read_support_format_zip_seekable(a) :
                                        archive_read_support_format_zip_streamable(a);
    if (rc != ARCHIVE_OK || archive_read_open_memory(a, zip.data, zip.len) != ARCHIVE_OK) {
        s = fg_fail(e, FG_PARSE, "Cannot open Binance ZIP");
        goto done;
    }
    while ((rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        la_int64_t declared = archive_entry_size(entry);
        size_t length = 0;
        la_ssize_t read_count;
        if (++entries != 1 || (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP ||
            archive_filter_code(a, 0) != ARCHIVE_FILTER_NONE || !path || strcmp(path, name) != 0 ||
            archive_entry_filetype(entry) != AE_IFREG || archive_entry_symlink(entry) ||
            archive_entry_hardlink(entry) || archive_entry_is_encrypted(entry) ||
            (archive_entry_size_is_set(entry) && declared < 0)) {
            s = fg_fail(e, FG_PARSE, "Unexpected, unsafe or unsupported Binance ZIP member");
            goto done;
        }
        if (archive_entry_size_is_set(entry) && (uint64_t)declared > limit) {
            s = fg_fail(e, FG_LIMIT, "Binance ZIP member exceeds decompression limit");
            goto done;
        }
        while ((read_count = archive_read_data(a, block, sizeof(block))) > 0) {
            size_t n = (size_t)read_count;
            if (n > limit - total) {
                s = fg_fail(e, FG_LIMIT, "Binance decompressed data exceeds limit");
                goto done;
            }
            total += n;
            if (seekable) {
                s = fg_buffer_append(csv, block, n, e);
                if (s != FG_OK) goto done;
            } else if (length > csv->len || n > csv->len - length ||
                       memcmp(csv->data + length, block, n) != 0) {
                s = fg_fail(e, FG_PARSE, "Inconsistent ZIP directory and local data");
                goto done;
            }
            length += n;
        }
        if (read_count != 0 ||
            (archive_entry_size_is_set(entry) && (uint64_t)declared != length) ||
            (!seekable && length != csv->len)) {
            s = fg_fail(e, FG_PARSE, "Corrupt Binance ZIP data, size or CRC");
            goto done;
        }
    }
    if (rc != ARCHIVE_EOF || entries != 1 || archive_read_has_encrypted_entries(a) > 0)
        s = fg_fail(e, FG_PARSE, "Corrupt or unsupported Binance ZIP directory");
done:
    if (archive_read_close(a) != ARCHIVE_OK && s == FG_OK)
        s = fg_fail(e, FG_PARSE, "Cannot finish Binance ZIP");
    if (archive_read_free(a) != ARCHIVE_OK && s == FG_OK)
        s = fg_fail(e, FG_PARSE, "Cannot release Binance ZIP reader");
    return s;
}

fg_status fg_binance_download(fg_client *client, const fg_binance_file *f,
    const fg_binance_limits *limits, fg_buffer *csv, fg_error *e)
{
    fg_binance_names names;
    fg_response zip = {0}, checksum = {0};
    fg_buffer result = {0};
    fg_status s;
    if (!client || !csv || (csv->data ? csv->capacity <= csv->len : csv->len || csv->capacity))
        return fg_fail(e, FG_INVALID, "Invalid Binance client or output buffer");
    s = fg_binance_build_names(f, &names, e);
    if (s != FG_OK) return s;
    s = get_archive(client, names.zip_url, &zip, e);
    if (s != FG_OK) goto done;
    if (zip.body.len > archive_limit(limits)) {
        s = fg_fail(e, FG_LIMIT, "Binance compressed archive exceeds limit");
        goto done;
    }
    s = get_archive(client, names.checksum_url, &checksum, e);
    if (s == FG_NOT_FOUND) {
        s = fg_fail(e, FG_CHECKSUM, "Binance checksum sidecar is missing");
        if (e) e->http_status = checksum.status;
    }
    if (s != FG_OK) goto done;
    if (checksum.body.len > 4096) {
        s = fg_fail(e, FG_LIMIT, "Binance checksum exceeds limit");
        goto done;
    }
    s = verify_digest(fg_s_view(&zip.body), fg_s_view(&checksum.body), names.zip_name, e);
    if (s != FG_OK) goto done;
    if (!zip_envelope(fg_s_view(&zip.body), names.csv_name)) {
        s = fg_fail(e, FG_PARSE, "Incomplete or unsupported Binance ZIP envelope");
        goto done;
    }
    s = unzip_pass(fg_s_view(&zip.body), names.csv_name, csv_limit(limits), 1, &result, e);
    if (s == FG_OK)
        s = unzip_pass(fg_s_view(&zip.body), names.csv_name, csv_limit(limits), 0, &result, e);
    if (s == FG_OK) {
        fg_buffer_free(csv);
        *csv = result;
        result = (fg_buffer){0};
    }
done:
    fg_response_free(&zip);
    fg_response_free(&checksum);
    fg_buffer_free(&result);
    return s;
}

typedef struct binance_csv {
    const fg_binance_file *file;
    binance_period period;
    fg_binance_callback callback;
    void *user;
    size_t rows, limit;
} binance_csv;

static fg_status number(fg_str text, int integer, uint64_t *out, fg_error *e)
{
    size_t i;
    fg_json *v = NULL;
    fg_status s;
    if (!text.len || text.data[0] == '-' || text.len > 256)
        return fg_fail(e, text.len > 256 ? FG_LIMIT : FG_PARSE, "Invalid Binance numeric field");
    if (integer) for (i = 0; i < text.len; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
            return fg_fail(e, FG_PARSE, "Binance integer must contain decimal digits only");
    s = fg_json_new_number(text, &v, e);
    if (s == FG_OK && integer) s = fg_json_u64(v, out, e);
    fg_json_free(v);
    if (s != FG_OK && s != FG_LIMIT && s != FG_NOMEM)
        return fg_fail(e, FG_PARSE, "Invalid Binance numeric field");
    return s;
}

static fg_status timestamp(fg_str text, const binance_period *p, int64_t *out, fg_error *e)
{
    uint64_t value, factor = p->microseconds ? 1 : 1000;
    fg_status s = number(text, 1, &value, e);
    if (s != FG_OK) return s;
    if (value > (uint64_t)INT64_MAX / factor)
        return fg_fail(e, FG_LIMIT, "Binance timestamp exceeds int64 microseconds");
    *out = (int64_t)(value * factor);
    return FG_OK;
}

static fg_status parse_boolean(fg_str text, int *out, fg_error *e)
{
    if (fg_s_iequal(text, FG_STR("true"))) *out = 1;
    else if (fg_s_iequal(text, FG_STR("false"))) *out = 0;
    else return fg_fail(e, FG_PARSE, "Invalid Binance boolean");
    return FG_OK;
}

static int column(fg_str s, const char *a, const char *b, const char *c)
{
    return fg_s_equal(s, a) || (b && fg_s_equal(s, b)) || (c && fg_s_equal(s, c));
}

static int is_header(const fg_binance_file *file, const fg_str *f, size_t n)
{
    int cm = file->market == FG_BINANCE_CM;
    if (file->kind == FG_BINANCE_TRADES) {
        return column(f[0],"id","trade_id",NULL) && fg_s_equal(f[1],"price") && fg_s_equal(f[2],"qty") &&
            column(f[3],cm ? "base_qty" : "quote_qty",cm ? "baseQty" : "quoteQty",NULL) &&
            fg_s_equal(f[4],"time") && column(f[5],"is_buyer_maker","isBuyerMaker",NULL) &&
            (n == 6 || column(f[6],"is_best_match","isBestMatch",NULL));
    }
    /* Binance's published CM klines label the base-asset columns quote_volume
       and taker_buy_quote_volume, exactly as UM does; the values stay base. */
    return column(f[0],"open_time","openTime",NULL) &&
        fg_s_equal(f[1],"open") && fg_s_equal(f[2],"high") && fg_s_equal(f[3],"low") &&
        fg_s_equal(f[4],"close") && fg_s_equal(f[5],"volume") &&
        column(f[6],"close_time","closeTime",NULL) &&
        column(f[7],cm ? "base_volume" : "quote_volume",cm ? "base_asset_volume" : "quote_asset_volume",
            cm ? "quote_volume" : NULL) &&
        column(f[8],"count","number_of_trades",NULL) &&
        column(f[9],"taker_buy_volume",cm ? NULL : "taker_buy_base_asset_volume",NULL) &&
        column(f[10],cm ? "taker_buy_base_volume" : "taker_buy_quote_volume",
            cm ? "taker_buy_base_asset_volume" : "taker_buy_quote_asset_volume",
            cm ? "taker_buy_quote_volume" : NULL) &&
        fg_s_equal(f[11],"ignore");
}

static fg_status csv_row(void *user, const fg_str *f, size_t n, size_t row, fg_error *e)
{
    binance_csv *c = (binance_csv *)user;
    fg_binance_record r = {0};
    fg_status s;
    size_t i;
    int64_t time;
    size_t expected = c->file->kind == FG_BINANCE_KLINES ? 12u :
                      c->file->market == FG_BINANCE_SPOT ? 7u : 6u;
    if (n != expected) return fg_fail(e, FG_PARSE, "Unexpected Binance CSV column count");
    if (row == 0 && is_header(c->file, f, n)) return FG_OK;
    if (c->rows >= c->limit) return fg_fail(e, FG_LIMIT, "Binance CSV row limit exceeded");
    r.kind = c->file->kind;
    r.fields = f;
    r.field_count = n;
    r.row_index = c->rows;
    if (r.kind == FG_BINANCE_KLINES) {
        fg_binance_kline *k = &r.value.kline;
        s = timestamp(f[0], &c->period, &k->open_time_us, e);
        if (s != FG_OK) return s;
        s = timestamp(f[6], &c->period, &k->close_time_us, e);
        if (s != FG_OK) return s;
        s = number(f[8], 1, &k->trade_count, e);
        if (s != FG_OK) return s;
        for (i = 1; i < 12; ++i) if (i != 6 && i != 8) {
            s = number(f[i], 0, NULL, e);
            if (s != FG_OK) return s;
        }
        if (k->close_time_us < k->open_time_us)
            return fg_fail(e, FG_PARSE, "Binance kline closes before it opens");
        k->open = f[1]; k->high = f[2]; k->low = f[3]; k->close = f[4]; k->volume = f[5];
        k->asset_volume = f[7]; k->taker_buy_volume = f[9]; k->taker_buy_asset_volume = f[10]; k->ignore = f[11];
        time = k->open_time_us;
    } else {
        fg_binance_trade *t = &r.value.trade;
        s = number(f[0], 1, &t->id, e);
        if (s != FG_OK) return s;
        for (i = 1; i <= 3; ++i) {
            s = number(f[i], 0, NULL, e);
            if (s != FG_OK) return s;
        }
        s = timestamp(f[4], &c->period, &t->time_us, e);
        if (s != FG_OK) return s;
        s = parse_boolean(f[5], &t->buyer_maker, e);
        if (s != FG_OK) return s;
        if (n == 7) {
            s = parse_boolean(f[6], &t->best_match, e);
            if (s != FG_OK) return s;
            t->has_best_match = 1;
        }
        t->price = f[1]; t->quantity = f[2]; t->asset_quantity = f[3];
        time = t->time_us;
    }
    if (time < c->period.begin_us || time >= c->period.end_us)
        return fg_fail(e, FG_PARSE, "Binance timestamp is outside its archive period");
    ++c->rows;
    return c->callback(c->user, c->file, &r, e);
}

fg_status fg_binance_parse_csv(const fg_binance_file *f, fg_str csv,
    const fg_binance_limits *limits, fg_binance_callback cb, void *user, fg_error *e)
{
    binance_csv c = {0};
    fg_status s;
    if (!f || !cb || (!csv.data && csv.len) ||
        (f->kind != FG_BINANCE_KLINES && f->kind != FG_BINANCE_TRADES))
        return fg_fail(e, FG_INVALID, "Typed Binance records require klines or trades");
    s = period_info(f, &c.period, e);
    if (s != FG_OK) return s;
    if (csv.len > csv_limit(limits)) return fg_fail(e, FG_LIMIT, "Binance CSV exceeds byte limit");
    c.file = f; c.callback = cb; c.user = user; c.limit = row_limit(limits);
    s = fg_csv_parse(csv, csv_row, &c, e);
    if (s == FG_OK && !c.rows) return fg_fail(e, FG_PARSE, "Binance CSV contains no data rows");
    return s;
}

fg_status fg_binance_fetch(fg_client *client, const fg_binance_file *f,
    const fg_binance_limits *limits, fg_binance_callback cb, void *user, fg_error *e)
{
    fg_buffer csv = {0};
    fg_status s;
    if (!f || !cb || (f->kind != FG_BINANCE_KLINES && f->kind != FG_BINANCE_TRADES))
        return fg_fail(e, FG_INVALID, "Typed Binance records require klines or trades");
    s = fg_binance_download(client, f, limits, &csv, e);
    if (s == FG_OK) s = fg_binance_parse_csv(f, fg_s_view(&csv), limits, cb, user, e);
    fg_buffer_free(&csv);
    return s;
}

fg_status fg_binance_fetch_window(fg_client *client, const fg_binance_window *w,
    const fg_binance_limits *limits, fg_binance_callback cb, void *user, fg_error *e)
{
    fg_binance_file f;
    binance_period p;
    int64_t start, end, day;
    size_t count = 0, i;
    char date[21];
    fg_status s;
    if (!client || !w || !cb ||
        (w->kind != FG_BINANCE_KLINES && w->kind != FG_BINANCE_TRADES) ||
        fg_s_date(w->start_date, &start, NULL) != FG_OK ||
        fg_s_date(w->end_date, &end, NULL) != FG_OK || start < 0 || end < start)
        return fg_fail(e, FG_INVALID, "Binance requires an explicit ordered date window");
    f = (fg_binance_file){w->market, w->kind, w->period, w->symbol, w->interval, {0}};
    day = start;
    do {
        s = fg_s_format_time(day, date, 1, e);
        if (s != FG_OK) return s;
        f.date = (fg_str){date, w->period == FG_BINANCE_MONTHLY ? 7u : 10u};
        s = period_info(&f, &p, e);
        if (s != FG_OK) return s;
        if (++count > file_limit(limits)) return fg_fail(e, FG_LIMIT, "Binance window exceeds archive limit");
        day = p.end_us / INT64_C(1000000);
    } while (day <= end);
    day = start;
    for (i = 0; i < count; ++i) {
        s = fg_s_format_time(day, date, 1, e);
        if (s != FG_OK) return s;
        f.date = (fg_str){date, w->period == FG_BINANCE_MONTHLY ? 7u : 10u};
        s = period_info(&f, &p, e);
        if (s != FG_OK) return s;
        s = fg_binance_fetch(client, &f, limits, cb, user, e);
        if (s != FG_OK) return s;
        day = p.end_us / INT64_C(1000000);
    }
    return FG_OK;
}
