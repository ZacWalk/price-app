#include <fg_binance.h>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define OK(x) CHECK((x) == FG_OK)
static const char *fixture_directory;

static fg_str view(const fg_buffer *b) { return (fg_str){b->data, b->len}; }
static fg_buffer fixture(const char *name)
{
    char path[1024];
    fg_buffer b = {0};
#ifdef _WIN32
    (void)snprintf(path, sizeof(path), "%s\\%s", fixture_directory, name);
#else
    (void)snprintf(path, sizeof(path), "%s/%s", fixture_directory, name);
#endif
    OK(fg_read_file(FG_STR(path), 1024*1024, &b, NULL));
    return b;
}

static fg_binance_file file_spec(fg_binance_market market, fg_binance_kind kind,
                                  fg_binance_period period, const char *date)
{
    fg_binance_file f = {market, kind, period, {0}, {0}, {0}};
    f.symbol = FG_STR(market == FG_BINANCE_CM ? "BTCUSD_PERP" : "ETHUSDT");
    f.interval = FG_STR(kind == FG_BINANCE_KLINES ? "1m" : "");
    f.date = FG_STR(date);
    return f;
}

typedef struct member {
    const char *name;
    fg_str data;
    unsigned int type;
} member;

static fg_buffer make_archive(const member *members, size_t count, int deflate, int tar)
{
    fg_buffer out = {0};
    struct archive *a = archive_write_new();
    size_t i, used = 0, capacity = 65536;
    CHECK(a != NULL);
    for (i = 0; i < count; ++i) capacity += members[i].data.len + 4096;
    out.data = (char *)malloc(capacity + 1);
    CHECK(out.data != NULL);
    out.capacity = capacity + 1;
    CHECK((tar ? archive_write_set_format_pax_restricted(a) : archive_write_set_format_zip(a)) == ARCHIVE_OK);
    if (!tar) CHECK(archive_write_set_options(a, deflate == 2 ? "zip:compression=deflate,zip:zip64" :
                                 deflate ? "zip:compression=deflate" : "zip:compression=store") == ARCHIVE_OK);
    CHECK(archive_write_set_bytes_per_block(a, 0) == ARCHIVE_OK);
    CHECK(archive_write_open_memory(a, out.data, capacity, &used) == ARCHIVE_OK);
    for (i = 0; i < count; ++i) {
        struct archive_entry *entry = archive_entry_new();
        CHECK(entry != NULL);
        archive_entry_set_pathname(entry, members[i].name);
        archive_entry_set_filetype(entry, members[i].type);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 0, 0);
        archive_entry_set_size(entry, members[i].type == AE_IFREG ? (la_int64_t)members[i].data.len : 0);
        if (members[i].type == AE_IFLNK) archive_entry_set_symlink(entry, "elsewhere.csv");
        CHECK(archive_write_header(a, entry) == ARCHIVE_OK);
        if (members[i].type == AE_IFREG)
            CHECK(archive_write_data(a, members[i].data.data, members[i].data.len) == (la_ssize_t)members[i].data.len);
        archive_entry_free(entry);
    }
    CHECK(archive_write_close(a) == ARCHIVE_OK);
    CHECK(archive_write_free(a) == ARCHIVE_OK);
    out.len = used;
    out.data[used] = 0;
    return out;
}

static fg_buffer zip_csv(const fg_binance_file *f, fg_str csv, int deflate)
{
    fg_binance_names names;
    member m;
    OK(fg_binance_build_names(f, &names, NULL));
    m = (member){names.csv_name, csv, AE_IFREG};
    return make_archive(&m, 1, deflate, 0);
}

static fg_buffer digest(fg_str zip, const char *name)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    char text[256], *p = text;
    size_t i;
    fg_buffer out = {0};
    CHECK(ctx != NULL);
    CHECK(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1);
    CHECK(EVP_DigestUpdate(ctx, zip.data, zip.len) == 1);
    CHECK(EVP_DigestFinal_ex(ctx, bytes, &n) == 1 && n == 32);
    EVP_MD_CTX_free(ctx);
    for (i = 0; i < n; ++i) {
        (void)snprintf(p, 3, "%02x", bytes[i]);
        p += 2;
    }
    (void)snprintf(p, sizeof(text) - 64, "  %s\n", name);
    OK(fg_buffer_set(&out, FG_STR(text), NULL));
    return out;
}

typedef struct step {
    const char *url;
    fg_str body;
    int http;
    fg_status status;
} step;
typedef struct mock {
    const step *steps;
    size_t count, next, max_bytes;
} mock;

static fg_status replay(void *user, const fg_request *r, fg_response *out, fg_error *e)
{
    mock *m = (mock *)user;
    const step *s;
    size_t i;
    CHECK(m->next < m->count);
    s = &m->steps[m->next++];
    CHECK(fg_str_equal(r->url, FG_STR(s->url)));
    CHECK(r->body.len == 0);
    CHECK(r->max_bytes == m->max_bytes);
    for (i = 0; i < r->header_count; ++i) {
        CHECK(!fg_str_equal(r->headers[i].name, FG_STR("Cookie")));
        CHECK(!fg_str_equal(r->headers[i].name, FG_STR("Authorization")));
    }
    out->status = s->http;
    OK(fg_buffer_set(&out->body, s->body, e));
    if (s->status != FG_OK) return fg_fail(e, s->status, "mock transport failed");
    return FG_OK;
}

static fg_client *new_client(mock *m, fg_transport *t, size_t max_bytes)
{
    fg_client_options o = {0};
    fg_client *c = NULL;
    *t = (fg_transport){m, replay, NULL};
    o.max_response_bytes = max_bytes;
    m->max_bytes = max_bytes ? max_bytes : (size_t)256*1024*1024;
    o.user_agent = FG_STR("binance-offline-tests");
    OK(fg_client_create(t, &o, &c, NULL));
    return c;
}

typedef struct result {
    size_t count;
    fg_binance_record last;
    fg_buffer decimal, asset;
    char date[11];
    fg_status failure;
} result;

static fg_status record(void *user, const fg_binance_file *f, const fg_binance_record *r, fg_error *e)
{
    result *out = (result *)user;
    CHECK(r->kind == f->kind);
    CHECK(r->fields != NULL);
    CHECK(r->field_count == (f->kind == FG_BINANCE_KLINES ? 12u : f->market == FG_BINANCE_SPOT ? 7u : 6u));
    CHECK(f->date.len < sizeof(out->date));
    memcpy(out->date, f->date.data, f->date.len);
    out->date[f->date.len] = 0;
    ++out->count;
    out->last = *r;
    OK(fg_buffer_set(&out->decimal, f->kind == FG_BINANCE_KLINES ? r->value.kline.open : r->value.trade.price, e));
    OK(fg_buffer_set(&out->asset, f->kind == FG_BINANCE_KLINES ? r->value.kline.asset_volume : r->value.trade.asset_quantity, e));
    if (out->failure) return fg_fail(e, out->failure, "consumer stopped");
    return FG_OK;
}

static void clear_result(result *r)
{
    fg_buffer_free(&r->decimal);
    fg_buffer_free(&r->asset);
    *r = (result){0};
}

static void fetch_case(const fg_binance_file *f, fg_str zip, const fg_str *checksum_override,
                        const fg_binance_limits *limits, fg_status expected, size_t rows)
{
    fg_binance_names names;
    fg_buffer checksum = {0};
    fg_transport transport;
    fg_client *client;
    fg_error e = {0};
    result out = {0};
    step steps[2];
    mock m;
    fg_status actual;
    OK(fg_binance_build_names(f, &names, NULL));
    if (!checksum_override) checksum = digest(zip, names.zip_name);
    steps[0] = (step){names.zip_url, zip, 200, FG_OK};
    steps[1] = (step){names.checksum_url, checksum_override ? *checksum_override : view(&checksum), 200, FG_OK};
    m = (mock){steps, 2, 0, 0};
    client = new_client(&m, &transport, 0);
    actual = fg_binance_fetch(client, f, limits, record, &out, &e);
    if (actual != expected)
        fprintf(stderr, "wanted %s, got %s: %s\n", fg_status_name(expected), fg_status_name(actual), e.message);
    CHECK(actual == expected);
    if (actual != FG_OK) CHECK(e.code == expected);
    CHECK(out.count == rows);
    CHECK(m.next == (limits && limits->max_archive_bytes && zip.len > limits->max_archive_bytes ? 1u : 2u));
    clear_result(&out);
    fg_client_free(client);
    fg_buffer_free(&checksum);
}

static void test_names(void)
{
    fg_binance_file f = file_spec(FG_BINANCE_UM, FG_BINANCE_TRADES, FG_BINANCE_DAILY, "2024-07-22");
    fg_binance_names n;
    const char *const invalid_symbols[] = {"", "../A", "/A", "A/B", "A\\B", "A?b", "A%2f", "a", "_A", "A:B", "A\n"};
    const char *const invalid_dates[] = {"2024-7-22", "2024-07", "2024-02-30", "2023-02-29", "0000-01-01",
                                        "1969-12-31", "2024-00-01", "2024-13-01", "2024-01-00", "2024-01-01/../"};
    const char *const valid_intervals[] = {"1s","1m","3m","5m","15m","30m","1h","2h","4h","6h","8h","12h","1d","3d","1w","1mo","1M"};
    size_t i;
    OK(fg_binance_build_names(&f, &n, NULL));
    CHECK(strcmp(n.zip_url, "https://data.binance.vision/data/futures/um/daily/trades/ETHUSDT/ETHUSDT-trades-2024-07-22.zip") == 0);
    CHECK(strcmp(n.csv_name, "ETHUSDT-trades-2024-07-22.csv") == 0);
    CHECK(strcmp(n.checksum_url, "https://data.binance.vision/data/futures/um/daily/trades/ETHUSDT/ETHUSDT-trades-2024-07-22.zip.CHECKSUM") == 0);
    f = file_spec(FG_BINANCE_SPOT, FG_BINANCE_KLINES, FG_BINANCE_MONTHLY, "2024-07");
    OK(fg_binance_build_names(&f, &n, NULL));
    CHECK(strcmp(n.zip_url, "https://data.binance.vision/data/spot/monthly/klines/ETHUSDT/1m/ETHUSDT-1m-2024-07.zip") == 0);
    f = file_spec(FG_BINANCE_CM, FG_BINANCE_KLINES, FG_BINANCE_DAILY, "2024-02-29");
    OK(fg_binance_build_names(&f, &n, NULL));
    CHECK(strcmp(n.zip_url, "https://data.binance.vision/data/futures/cm/daily/klines/BTCUSD_PERP/1m/BTCUSD_PERP-1m-2024-02-29.zip") == 0);
    for (i = 0; i < sizeof(invalid_symbols)/sizeof(*invalid_symbols); ++i) {
        f.symbol = FG_STR(invalid_symbols[i]);
        CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    }
    f.symbol = FG_STR("BTCUSD_250328");
    for (i = 0; i < sizeof(invalid_dates)/sizeof(*invalid_dates); ++i) {
        f.date = FG_STR(invalid_dates[i]);
        CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    }
    f.date = FG_STR("2024-02-29");
    f.symbol = (fg_str){"ABC\0DEF", 7};
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.symbol = FG_STR("BTCUSD_250328");
    f.interval = FG_STR("../1m");
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.interval = FG_STR("1s");
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.market = FG_BINANCE_SPOT;
    for (i = 0; i < sizeof(valid_intervals)/sizeof(*valid_intervals); ++i) {
        f.interval = FG_STR(valid_intervals[i]);
        OK(fg_binance_build_names(&f, &n, NULL));
    }
    f.kind = FG_BINANCE_TRADES;
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.interval = (fg_str){0};
    OK(fg_binance_build_names(&f, &n, NULL));
    f.market = (fg_binance_market)3;
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.market = FG_BINANCE_SPOT; f.kind = (fg_binance_kind)5;
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    f.kind = FG_BINANCE_TRADES; f.period = (fg_binance_period)2;
    CHECK(fg_binance_build_names(&f, &n, NULL) == FG_INVALID);
    CHECK(fg_binance_build_names(NULL, &n, NULL) == FG_INVALID);
    CHECK(fg_binance_build_names(&f, NULL, NULL) == FG_INVALID);
}

static void test_fixtures(void)
{
    static const struct {
        const char *name, *date;
        fg_binance_market market;
        fg_binance_kind kind;
        int64_t timestamp;
    } cases[] = {
        {"spot-trades-2024.csv","2024-07-22",FG_BINANCE_SPOT,FG_BINANCE_TRADES,INT64_C(1721606400123000)},
        {"spot-trades-2025.csv","2025-01-01",FG_BINANCE_SPOT,FG_BINANCE_TRADES,INT64_C(1735689600123456)},
        {"um-trades-2025.csv","2025-01-01",FG_BINANCE_UM,FG_BINANCE_TRADES,INT64_C(1735689600123000)},
        {"cm-trades-2025.csv","2025-01-01",FG_BINANCE_CM,FG_BINANCE_TRADES,INT64_C(1735689600123000)},
        {"spot-klines-2025.csv","2025-01-01",FG_BINANCE_SPOT,FG_BINANCE_KLINES,INT64_C(1735689600000123)},
        {"um-klines-2025.csv","2025-01-01",FG_BINANCE_UM,FG_BINANCE_KLINES,INT64_C(1735689600000000)},
        {"cm-klines-2025.csv","2025-01-01",FG_BINANCE_CM,FG_BINANCE_KLINES,INT64_C(1735689600000000)},
        /* Header as Binance publishes CM klines: quote_* names, base-asset values. */
        {"cm-klines-published-2025.csv","2025-01-01",FG_BINANCE_CM,FG_BINANCE_KLINES,INT64_C(1735689600000000)}
    };
    size_t i;
    int compression, period;
    for (i = 0; i < sizeof(cases)/sizeof(*cases); ++i) {
        fg_buffer csv = fixture(cases[i].name);
        for (period = 0; period <= 1; ++period) for (compression = 0; compression <= 2; ++compression) {
            fg_binance_file f = file_spec(cases[i].market, cases[i].kind, (fg_binance_period)period, cases[i].date);
            fg_binance_names names;
            fg_buffer zip, checksum, downloaded = {0};
            step steps[2];
            mock m;
            fg_transport t;
            fg_client *c;
            result r = {0};
            if (period) f.date.len = 7;
            OK(fg_binance_build_names(&f, &names, NULL));
            zip = zip_csv(&f, view(&csv), compression);
            checksum = digest(view(&zip), names.zip_name);
            if (compression) {
                size_t j;
                checksum.data[65] = '*';
                for (j = 0; j < 64; ++j)
                    if (checksum.data[j] >= 'a' && checksum.data[j] <= 'f') checksum.data[j] -= 'a'-'A';
            }
            steps[0] = (step){names.zip_url, view(&zip), 200, FG_OK};
            steps[1] = (step){names.checksum_url, view(&checksum), 200, FG_OK};
            m = (mock){steps, 2, 0, 0}; c = new_client(&m, &t, 0);
            OK(fg_binance_download(c, &f, NULL, &downloaded, NULL));
            CHECK(m.next == 2 && fg_str_equal(view(&downloaded), view(&csv)));
            OK(fg_binance_parse_csv(&f, view(&downloaded), NULL, record, &r, NULL));
            CHECK(r.count == 1 && r.last.row_index == 0);
            if (f.kind == FG_BINANCE_TRADES) {
                CHECK(r.last.value.trade.time_us == cases[i].timestamp);
                CHECK(r.last.value.trade.has_best_match == (f.market == FG_BINANCE_SPOT));
                CHECK(r.last.value.trade.id == (i == 0 ? UINT64_MAX : UINT64_C(9007199254740993)));
                CHECK(r.last.value.trade.buyer_maker == (i == 0 || i == 2));
                if (f.market == FG_BINANCE_SPOT) CHECK(r.last.value.trade.best_match == (i == 1));
                CHECK(fg_str_equal(view(&r.decimal), FG_STR(i == 3 ? "9635.0" : "0.123456789012345678901234567890")));
                if (i == 3) CHECK(fg_str_equal(view(&r.asset), FG_STR("0.01037883")));
            } else {
                CHECK(r.last.value.kline.open_time_us == cases[i].timestamp);
                CHECK(r.last.value.kline.close_time_us == (i == 4 ? INT64_C(1735689659999999) : INT64_C(1735689659999000)));
                CHECK(r.last.value.kline.trade_count == (i >= 6 ? 48u : UINT64_MAX));
                CHECK(fg_str_equal(view(&r.decimal), FG_STR(i >= 6 ? "9635.0" : "4.15070000000000000000000001")));
                if (i >= 6) CHECK(fg_str_equal(view(&r.asset), FG_STR("2.13660389")));
            }
            fg_client_free(c); clear_result(&r);
            fg_buffer_free(&zip); fg_buffer_free(&checksum); fg_buffer_free(&downloaded);
        }
        fg_buffer_free(&csv);
    }
}

static size_t signature(fg_str bytes, const char *sig)
{
    size_t i;
    for (i = 0; i + 4 <= bytes.len; ++i) if (!memcmp(bytes.data+i, sig, 4)) return i;
    CHECK(0);
    return 0;
}

static unsigned int little16(const char *p)
{ return (unsigned char)p[0] | ((unsigned int)(unsigned char)p[1] << 8); }

static void test_archives(void)
{
    fg_binance_file f = file_spec(FG_BINANCE_SPOT, FG_BINANCE_TRADES, FG_BINANCE_DAILY, "2025-01-01");
    fg_binance_names names;
    fg_buffer csv = fixture("spot-trades-2025.csv"), zip, checksum;
    fg_str override;
    const char *const paths[] = {
        "../ETHUSDT-trades-2025-01-01.csv", "/ETHUSDT-trades-2025-01-01.csv",
        "C:\\ETHUSDT-trades-2025-01-01.csv", "\\\\host\\share\\ETHUSDT-trades-2025-01-01.csv",
        "sub/ETHUSDT-trades-2025-01-01.csv", "./ETHUSDT-trades-2025-01-01.csv",
        "sub\\..\\ETHUSDT-trades-2025-01-01.csv", "wrong.csv", "ETHUSDT-trades-2025-01-01.csv.exe"
    };
    member members[2];
    size_t i;
    int compression;
    OK(fg_binance_build_names(&f, &names, NULL));
    for (compression = 0; compression <= 1; ++compression) {
        zip = zip_csv(&f, view(&csv), compression);
        checksum = digest(view(&zip), names.zip_name);
        override = view(&checksum);
        fetch_case(&f, view(&zip), &override, NULL, FG_OK, 1);
        zip.data[0] ^= 1; /* Incorrect checksum must win over invalid ZIP magic. */
        fetch_case(&f, view(&zip), &override, NULL, FG_CHECKSUM, 0);
        zip.data[0] ^= 1;
        --checksum.len; checksum.data[checksum.len] = 0;
        override = view(&checksum);
        fetch_case(&f, view(&zip), &override, NULL, FG_OK, 1);
        OK(fg_buffer_append(&checksum, "\r\n", 2, NULL));
        override = view(&checksum);
        fetch_case(&f, view(&zip), &override, NULL, FG_OK, 1);
        checksum.data[0] = 'g';
        fetch_case(&f, view(&zip), &override, NULL, FG_CHECKSUM, 0);
        fg_buffer_free(&checksum);
        checksum = digest(view(&zip), "OTHER.zip");
        override = view(&checksum);
        fetch_case(&f, view(&zip), &override, NULL, FG_CHECKSUM, 0);
        fg_buffer_free(&checksum);
        checksum = digest(view(&zip), names.zip_name);
        OK(fg_buffer_append(&checksum, "\nextra", 6, NULL));
        override = view(&checksum);
        fetch_case(&f, view(&zip), &override, NULL, FG_CHECKSUM, 0);
        fg_buffer_free(&checksum);
        {
            const char *const bad[] = {"", "deadbeef", "SHA256 (ETHUSDT.zip) = bad",
                                      "0\n", "000000000000000000000000000000000000000000000000000000000000000"};
            for (i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
                override = FG_STR(bad[i]);
                fetch_case(&f, view(&zip), &override, NULL, FG_CHECKSUM, 0);
            }
        }
        {
            size_t central = signature(view(&zip), "PK\001\002");
            char original = zip.data[central+16];
            zip.data[central+16] ^= 1; /* Signed archive with bad central CRC. */
            fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
            zip.data[central+16] = original;
        }
        {
            size_t local = signature(view(&zip), "PK\003\004");
            size_t offset = local+30+little16(zip.data+local+26)+little16(zip.data+local+28);
            char original = zip.data[offset];
            zip.data[offset] ^= 64; /* Corrupt stored payload / deflate stream. */
            fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
            zip.data[offset] = original;
        }
        {
            size_t descriptor = signature(view(&zip), "PK\007\010");
            char original = zip.data[descriptor+4];
            zip.data[descriptor+4] ^= 1;
            fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
            zip.data[descriptor+4] = original;
        }
        {
            fg_binance_limits l = {0};
            l.max_archive_bytes = zip.len-1;
            fetch_case(&f, view(&zip), NULL, &l, FG_LIMIT, 0);
            l.max_archive_bytes = zip.len;
            l.max_csv_bytes = csv.len-1;
            fetch_case(&f, view(&zip), NULL, &l, FG_LIMIT, 0);
            l.max_csv_bytes = csv.len;
            fetch_case(&f, view(&zip), NULL, &l, FG_OK, 1);
        }
        for (i = 1; i <= 24; ++i)
            fetch_case(&f, (fg_str){zip.data, zip.len-i}, NULL, NULL, FG_PARSE, 0);
        OK(fg_buffer_append(&zip, "junk", 4, NULL));
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
        fg_buffer_free(&zip);
    }
    for (i = 0; i < sizeof(paths)/sizeof(*paths); ++i) {
        members[0] = (member){paths[i], view(&csv), AE_IFREG};
        zip = make_archive(members, 1, 0, 0);
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
        fg_buffer_free(&zip);
    }
    for (i = 0; i < 2; ++i) {
        members[0] = (member){names.csv_name, view(&csv), AE_IFREG};
        members[1] = (member){i ? "other.csv" : names.csv_name, view(&csv), AE_IFREG};
        zip = make_archive(members, 2, 1, 0);
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
        fg_buffer_free(&zip);
    }
    for (i = 0; i < 2; ++i) {
        members[0] = (member){names.csv_name, {0}, i ? AE_IFDIR : AE_IFLNK};
        zip = make_archive(members, 1, 0, 0);
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
        fg_buffer_free(&zip);
    }
    members[0] = (member){names.csv_name, view(&csv), AE_IFREG};
    zip = make_archive(members, 1, 0, 1);
    fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
    fg_buffer_free(&zip);
    zip = make_archive(NULL, 0, 0, 0);
    fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
    fg_buffer_free(&zip);
    /* Encrypted, unsupported compression, nonregular mode, mismatched names. */
    for (i = 0; i < 4; ++i) {
        size_t central;
        zip = zip_csv(&f, view(&csv), 0);
        central = signature(view(&zip), "PK\001\002");
        if (i == 0) { zip.data[6] |= 1; zip.data[central+8] |= 1; }
        if (i == 1) { zip.data[8] = 99; zip.data[central+10] = 99; }
        if (i == 2) { zip.data[central+5] = 3; zip.data[central+41] = 0x10; }
        if (i == 3) zip.data[30] = 'X';
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
        fg_buffer_free(&zip);
    }
    /* Raw NUL-bearing member names must not pass libarchive's C-string view. */
    zip = zip_csv(&f, view(&csv), 0);
    {
        size_t central = signature(view(&zip), "PK\001\002");
        zip.data[31] = 0; zip.data[central+47] = 0;
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 0);
    }
    fg_buffer_free(&zip);
    {
        fg_buffer bomb = {0};
        fg_binance_limits l = {0};
        char block[4096];
        memset(block, 'x', sizeof(block));
        for (i = 0; i < 128; ++i) OK(fg_buffer_append(&bomb, block, sizeof(block), NULL));
        zip = zip_csv(&f, view(&bomb), 1);
        CHECK(zip.len < 4096);
        l.max_csv_bytes = 4096;
        fetch_case(&f, view(&zip), NULL, &l, FG_LIMIT, 0);
        fg_buffer_free(&zip); fg_buffer_free(&bomb);
    }
    fg_buffer_free(&csv);
}

static void parse_case(const fg_binance_file *f, fg_str csv, fg_status expected, size_t rows)
{
    result r = {0};
    fg_error e = {0};
    fg_status actual = fg_binance_parse_csv(f, csv, NULL, record, &r, &e);
    if (actual != expected) fprintf(stderr, "parse wanted %s got %s: %s\n", fg_status_name(expected), fg_status_name(actual), e.message);
    CHECK(actual == expected);
    CHECK(r.count == rows);
    clear_result(&r);
}

static void test_csv(void)
{
    fg_binance_file f = file_spec(FG_BINANCE_SPOT, FG_BINANCE_TRADES, FG_BINANCE_DAILY, "2025-01-01");
    const char *const invalid[] = {
        "", "\n\n", "id,price,qty,quoteQty,time,isBuyerMaker,isBestMatch\n",
        "id,qty,price,quoteQty,time,isBuyerMaker,isBestMatch\n",
        "id,price,qty,baseQty,time,isBuyerMaker,isBestMatch\n",
        "1,1,1,1,1735689600000000,true\n",
        "1,1,1,1,1735689600000000,true,true,extra\n",
        "1,1,1,1,1735689600000000,true,true,\n",
        "1,1,1,1,1735689600000000,true,true\r",
        "1,\"1,1,1,1735689600000000,true,true\n",
        "1,NaN,1,1,1735689600000000,true,true\n",
        "1,Infinity,1,1,1735689600000000,true,true\n",
        "1,1x,1,1,1735689600000000,true,true\n",
        "1,01,1,1,1735689600000000,true,true\n",
        "1,+1,1,1,1735689600000000,true,true\n",
        "1,-1,1,1,1735689600000000,true,true\n",
        "1,1,1,1,1735689600000000,1,true\n",
        "1,1,1,1,1735689600000000,true,0\n",
        "1,1,1,1,1735689600000000,true,\n",
        "-1,1,1,1,1735689600000000,true,true\n",
        "1.0,1,1,1,1735689600000000,true,true\n",
        "1e2,1,1,1,1735689600000000,true,true\n",
        "1,1,1,1,1735689600000000.1,true,true\n",
        "1,1,1,1,1735689600000000e0,true,true\n",
        "1,1,1,1,1735689600000,true,true\n",
        "1,1,1,1,1735776000000000,true,true\n",
        "1,1,1,1,1735689599999999,true,true\n",
        "1,\"1\"garbage,1,1,1735689600000000,true,true\n"
    };
    size_t i;
    for (i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) parse_case(&f, FG_STR(invalid[i]), FG_PARSE, 0);
    parse_case(&f, FG_STR("18446744073709551616,1,1,1,1735689600000000,true,true"), FG_LIMIT, 0);
    parse_case(&f, FG_STR("1,1,1,1,9223372036854775808,true,true"), FG_LIMIT, 0);
    parse_case(&f, FG_STR("1,1,1,1,18446744073709551616,true,true"), FG_LIMIT, 0);
    parse_case(&f, FG_STR("\xef\xbb\xbf\"1\",\"1.00000000000000000001\",1,1,1735689600000001,TRUE,FALSE\r\n"), FG_OK, 1);
    parse_case(&f, FG_STR("1,1e-1000,1e+1000,0.000000000000000000000000000001,1735689600000001,false,true"), FG_OK, 1);
    {
        const char nul[] = "1,1,1,1,1735689600000000,true,true\0\n";
        const char utf8[] = "1,\xff,1,1,1735689600000000,true,true\n";
        parse_case(&f, (fg_str){nul, sizeof(nul)-1}, FG_PARSE, 0);
        parse_case(&f, (fg_str){utf8, sizeof(utf8)-1}, FG_PARSE, 0);
    }
    {
        fg_buffer csv = {0}, zip;
        const char *good = "1,1,1,1,1735689600000000,true,true\n";
        fg_binance_limits l = {0};
        result r = {0};
        fg_error e = {0};
        OK(fg_buffer_set(&csv, FG_STR(good), NULL));
        OK(fg_buffer_append(&csv, "bad trailing record\n", 20, NULL));
        parse_case(&f, view(&csv), FG_PARSE, 1);
        zip = zip_csv(&f, view(&csv), 1);
        fetch_case(&f, view(&zip), NULL, NULL, FG_PARSE, 1);
        fg_buffer_free(&zip);
        OK(fg_buffer_set(&csv, FG_STR(good), NULL));
        OK(fg_buffer_append(&csv, good, strlen(good), NULL));
        l.max_rows = 1;
        CHECK(fg_binance_parse_csv(&f, view(&csv), &l, record, &r, &e) == FG_LIMIT && r.count == 1);
        clear_result(&r);
        r.failure = FG_IO;
        CHECK(fg_binance_parse_csv(&f, view(&csv), NULL, record, &r, &e) == FG_IO);
        CHECK(r.count == 1 && strcmp(e.message, "consumer stopped") == 0);
        clear_result(&r);
        l.max_csv_bytes = csv.len-1;
        CHECK(fg_binance_parse_csv(&f, view(&csv), &l, record, &r, &e) == FG_LIMIT && r.count == 0);
        CHECK(fg_binance_parse_csv(&f, view(&csv), NULL, NULL, &r, &e) == FG_INVALID);
        CHECK(fg_binance_parse_csv(&f, (fg_str){NULL,1}, NULL, record, &r, &e) == FG_INVALID);
        fg_buffer_free(&csv);
    }
    {
        fg_buffer csv = {0};
        char long_number[257];
        memset(long_number, '1', sizeof(long_number));
        OK(fg_buffer_append(&csv, "1,", 2, NULL));
        OK(fg_buffer_append(&csv, long_number, sizeof(long_number), NULL));
        OK(fg_buffer_append(&csv, ",1,1,1735689600000000,true,true",
                            strlen(",1,1,1735689600000000,true,true"), NULL));
        parse_case(&f, view(&csv), FG_LIMIT, 0);
        fg_buffer_free(&csv);
    }
    /* Greater than 2^53: retain a microsecond that cannot survive a double. */
    {
        result r = {0};
        f.date = FG_STR("2255-06-05");
        OK(fg_binance_parse_csv(&f, FG_STR("1,1,1,1,9007199254740993,true,true"), NULL, record, &r, NULL));
        CHECK(r.last.value.trade.time_us == INT64_C(9007199254740993));
        clear_result(&r);
    }
    f = file_spec(FG_BINANCE_UM, FG_BINANCE_TRADES, FG_BINANCE_DAILY, "2025-01-01");
    parse_case(&f, FG_STR("1,1,1,1,1735689600000,true"), FG_OK, 1);
    parse_case(&f, FG_STR("1,1,1,1,1735689600000000,true"), FG_PARSE, 0);
    parse_case(&f, FG_STR("1,1,1,1,9223372036854776,true"), FG_LIMIT, 0);
    parse_case(&f, FG_STR("1,1,1,1,1735689600000,true,true"), FG_PARSE, 0);
    f.market = FG_BINANCE_CM;
    parse_case(&f, FG_STR("id,price,qty,quote_qty,time,is_buyer_maker\n1,1,1,1,1735689600000,true"), FG_PARSE, 0);
    f = file_spec(FG_BINANCE_SPOT, FG_BINANCE_KLINES, FG_BINANCE_DAILY, "2024-12-31");
    parse_case(&f, FG_STR("1735603200000,1,1,1,1,1,1735689599999,1,1,1,1,0"), FG_OK, 1);
    f.date = FG_STR("2025-01-01");
    parse_case(&f, FG_STR("1735689600000001,1,1,1,1,1,1735689600000000,1,1,1,1,0"), FG_PARSE, 0);
    parse_case(&f, FG_STR("1735689600000000,1,1,1,1,1,1735689600000001,1,18446744073709551616,1,1,0"), FG_LIMIT, 0);
    parse_case(&f, FG_STR("1735689600000000,1,1,1,1,1,1735689600000001,1,1,1,1,NaN"), FG_PARSE, 0);
    parse_case(&f, FG_STR("1735689600000000,1,1,1,1,1,1735689600000001,1,1.1,1,1,0"), FG_PARSE, 0);
}

static void test_http(void)
{
    fg_binance_file f = file_spec(FG_BINANCE_SPOT, FG_BINANCE_TRADES, FG_BINANCE_DAILY, "2025-01-01");
    fg_binance_names names;
    fg_buffer csv = fixture("spot-trades-2025.csv"), zip, checksum, output = {0};
    const struct { int http; fg_status transport, expected; } cases[] = {
        {404,FG_OK,FG_NOT_FOUND}, {500,FG_OK,FG_HTTP}, {403,FG_OK,FG_HTTP},
        {429,FG_OK,FG_HTTP}, {302,FG_OK,FG_HTTP}, {204,FG_OK,FG_HTTP}, {206,FG_OK,FG_HTTP},
        {0,FG_IO,FG_IO}, {404,FG_IO,FG_IO}, {200,FG_LIMIT,FG_LIMIT}
    };
    size_t i, which;
    OK(fg_binance_build_names(&f, &names, NULL));
    zip = zip_csv(&f, view(&csv), 0); checksum = digest(view(&zip), names.zip_name);
    for (i = 0; i < sizeof(cases)/sizeof(*cases); ++i) for (which = 0; which < 2; ++which) {
        step steps[2] = {{names.zip_url, {0}, 200, FG_OK}, {names.checksum_url, {0}, 200, FG_OK}};
        mock m = {steps, 2, 0, 0};
        fg_transport t;
        fg_client *c = new_client(&m, &t, 0);
        fg_error e = {0};
        result r = {0};
        fg_status expected = which == 1 && cases[i].expected == FG_NOT_FOUND
            ? FG_CHECKSUM : cases[i].expected;
        steps[0].body = view(&zip); steps[1].body = view(&checksum);
        steps[which].http = cases[i].http; steps[which].status = cases[i].transport;
        CHECK(fg_binance_fetch(c, &f, NULL, record, &r, &e) == expected);
        CHECK(e.code == expected && e.http_status == cases[i].http && r.count == 0);
        CHECK(m.next == which+1);
        fg_client_free(c);
    }
    {
        step steps[2] = {{names.zip_url, view(&zip), 200, FG_OK},
            {names.checksum_url, {0}, 404, FG_OK}};
        mock m = {steps, 2, 0, 0};
        fg_transport t;
        fg_client *c = new_client(&m, &t, 0);
        fg_error e = {0};
        OK(fg_buffer_set(&output, FG_STR("preserve me"), NULL));
        CHECK(fg_binance_download(c, &f, NULL, &output, &e) == FG_CHECKSUM);
        CHECK(m.next == 2 && e.http_status == 404 && e.code == FG_CHECKSUM);
        CHECK(fg_str_equal(view(&output), FG_STR("preserve me")));
        fg_client_free(c);
    }
    {
        step steps[2] = {{names.zip_url, {0}, 200, FG_OK}, {names.checksum_url, {0}, 200, FG_OK}};
        mock m = {steps, 2, 0, 0};
        fg_transport t;
        fg_client *c = new_client(&m, &t, zip.len-1);
        steps[0].body = view(&zip); steps[1].body = view(&checksum);
        OK(fg_buffer_set(&output, FG_STR("preserve me"), NULL));
        CHECK(fg_binance_download(c, &f, NULL, &output, NULL) == FG_LIMIT);
        CHECK(m.next == 1 && fg_str_equal(view(&output), FG_STR("preserve me")));
        fg_client_free(c);
    }
    {
        step steps[2] = {{names.zip_url, {0}, 200, FG_OK}, {names.checksum_url, {0}, 200, FG_OK}};
        mock m = {steps, 2, 0, 0};
        fg_transport t;
        fg_client *c = new_client(&m, &t, 0);
        char large[4097];
        memset(large, 'a', sizeof(large));
        steps[0].body = view(&zip); steps[1].body = (fg_str){large, sizeof(large)};
        CHECK(fg_binance_download(c, &f, NULL, &output, NULL) == FG_LIMIT);
        CHECK(m.next == 2 && fg_str_equal(view(&output), FG_STR("preserve me")));
        fg_client_free(c);
    }
    fg_buffer_free(&csv); fg_buffer_free(&zip); fg_buffer_free(&checksum); fg_buffer_free(&output);
}

static void test_windows(void)
{
    int monthly;
    for (monthly = 0; monthly <= 1; ++monthly) {
        fg_binance_file files[2] = {
            file_spec(FG_BINANCE_UM, FG_BINANCE_TRADES, (fg_binance_period)monthly, monthly ? "2024-12" : "2024-12-31"),
            file_spec(FG_BINANCE_UM, FG_BINANCE_TRADES, (fg_binance_period)monthly, monthly ? "2025-01" : "2025-01-01")
        };
        fg_binance_window w = {FG_BINANCE_UM, FG_BINANCE_TRADES, (fg_binance_period)monthly, {0}, {0}, {0}, {0}};
        fg_binance_names names[2];
        fg_buffer zip[2], checksum[2];
        step steps[4];
        mock m = {steps, 4, 0, 0};
        fg_transport t;
        fg_client *c;
        result r = {0};
        fg_binance_limits l = {0};
        size_t i;
        w.symbol = FG_STR("ETHUSDT");
        w.start_date = FG_STR("2024-12-31"); w.end_date = FG_STR("2025-01-01");
        for (i = 0; i < 2; ++i) {
            OK(fg_binance_build_names(&files[i], &names[i], NULL));
            zip[i] = zip_csv(&files[i], FG_STR(monthly ?
                (i ? "1,1,1,1,1738281600001,true\n" : "1,1,1,1,1733011200001,true\n") :
                (i ? "1,1,1,1,1735689600001,true\n" : "1,1,1,1,1735603200001,true\n")), 1);
            checksum[i] = digest(view(&zip[i]), names[i].zip_name);
            steps[2*i] = (step){names[i].zip_url, view(&zip[i]), 200, FG_OK};
            steps[2*i+1] = (step){names[i].checksum_url, view(&checksum[i]), 200, FG_OK};
        }
        c = new_client(&m, &t, 0);
        l.max_archives = 1;
        CHECK(fg_binance_fetch_window(c, &w, &l, record, &r, NULL) == FG_LIMIT);
        CHECK(m.next == 0);
        l.max_archives = 2;
        OK(fg_binance_fetch_window(c, &w, &l, record, &r, NULL));
        CHECK(m.next == 4 && r.count == 2 && strcmp(r.date, monthly ? "2025-01" : "2025-01-01") == 0);
        clear_result(&r); m.next = 0;
        steps[2].http = 404;
        CHECK(fg_binance_fetch_window(c, &w, &l, record, &r, NULL) == FG_NOT_FOUND);
        CHECK(m.next == 3 && r.count == 1);
        clear_result(&r); m.next = 0;
        r.failure = FG_IO;
        CHECK(fg_binance_fetch_window(c, &w, &l, record, &r, NULL) == FG_IO);
        CHECK(m.next == 2 && r.count == 1);
        clear_result(&r); m.next = 0;
        w.start_date = FG_STR("2025-01-02");
        CHECK(fg_binance_fetch_window(c, &w, NULL, record, &r, NULL) == FG_INVALID && m.next == 0);
        w.start_date = FG_STR("");
        CHECK(fg_binance_fetch_window(c, &w, NULL, record, &r, NULL) == FG_INVALID && m.next == 0);
        w.start_date = FG_STR("2024-02-30");
        CHECK(fg_binance_fetch_window(c, &w, NULL, record, &r, NULL) == FG_INVALID && m.next == 0);
        fg_client_free(c);
        for (i = 0; i < 2; ++i) { fg_buffer_free(&zip[i]); fg_buffer_free(&checksum[i]); }
    }
}

static void test_download_only_kinds(void)
{
    static const fg_binance_kind kinds[] = {
        FG_BINANCE_AGG_TRADES, FG_BINANCE_FUNDING_RATE, FG_BINANCE_METRICS
    };
    static const char *const names[] = {"aggTrades", "fundingRate", "metrics"};
    size_t i;
    int monthly;
    for (i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) for (monthly = 0; monthly < 2; ++monthly) {
        fg_binance_file f = file_spec(FG_BINANCE_UM, kinds[i],
            (fg_binance_period)monthly, monthly ? "2024-07" : "2024-07-22");
        fg_binance_names n;
        char expected[512];
        fg_buffer zip, checksum, output = {0};
        fg_str csv = FG_STR("provider,column\nexact,1.234567890123456789\n");
        fg_transport transport;
        fg_client *client;
        fg_error error = {0};
        step steps[2];
        mock m;
        result records = {0};
        fg_binance_window window = {FG_BINANCE_UM, kinds[i], FG_BINANCE_DAILY,
            {0}, {0}, {0}, {0}};
        OK(fg_binance_build_names(&f, &n, NULL));
        (void)snprintf(expected, sizeof(expected),
            "https://data.binance.vision/data/futures/um/%s/%s/ETHUSDT/ETHUSDT-%s-%s.zip",
            monthly ? "monthly" : "daily", names[i], names[i], monthly ? "2024-07" : "2024-07-22");
        CHECK(strcmp(n.zip_url, expected) == 0);
        (void)snprintf(expected, sizeof(expected), "ETHUSDT-%s-%s.csv",
            names[i], monthly ? "2024-07" : "2024-07-22");
        CHECK(strcmp(n.csv_name, expected) == 0);
        zip = zip_csv(&f, csv, 1);
        checksum = digest(view(&zip), n.zip_name);
        steps[0] = (step){n.zip_url, view(&zip), 200, FG_OK};
        steps[1] = (step){n.checksum_url, view(&checksum), 200, FG_OK};
        m = (mock){steps, 2, 0, 0};
        client = new_client(&m, &transport, 0);
        CHECK(fg_binance_parse_csv(&f, csv, NULL, record, &records, &error) == FG_INVALID);
        CHECK(fg_binance_fetch(client, &f, NULL, record, &records, &error) == FG_INVALID);
        window.symbol = f.symbol;
        window.start_date = FG_STR("2024-07-22");
        window.end_date = window.start_date;
        CHECK(fg_binance_fetch_window(client, &window, NULL, record, &records, &error) == FG_INVALID);
        CHECK(m.next == 0 && records.count == 0);
        OK(fg_binance_download(client, &f, NULL, &output, &error));
        CHECK(m.next == 2 && fg_str_equal(view(&output), csv));
        m.next = 0;
        checksum.data[0] = checksum.data[0] == '0' ? '1' : '0';
        CHECK(fg_binance_download(client, &f, NULL, &output, &error) == FG_CHECKSUM);
        CHECK(m.next == 2 && fg_str_equal(view(&output), csv));
        f.interval = FG_STR("1m");
        CHECK(fg_binance_build_names(&f, &n, &error) == FG_INVALID);
        fg_client_free(client);
        fg_buffer_free(&zip);
        fg_buffer_free(&checksum);
        fg_buffer_free(&output);
    }
}

int main(int argc, char **argv)
{
    fixture_directory = argc > 1 ? argv[1] : "tests/fixtures/binance";
    test_names();
    test_download_only_kinds();
    test_fixtures();
    test_archives();
    test_csv();
    test_http();
    test_windows();
    printf("Binance: %d checks passed (offline, stored/deflated ZIP, EVP SHA256)\n", checks);
    return 0;
}
