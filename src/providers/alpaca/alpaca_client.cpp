#include "options/providers/alpaca/alpaca_client.hpp"
#include "options/providers/alpaca/alpaca_json.hpp"
#include "options/providers/alpaca/option_json.hpp"

#include <curl/curl.h>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace options::providers::alpaca {
namespace {

struct CurlCleanup { void operator()(CURL* value) const { curl_easy_cleanup(value); } };
struct HeaderCleanup { void operator()(curl_slist* value) const { curl_slist_free_all(value); } };

std::size_t append_body(char* contents, std::size_t size, std::size_t count, void* destination) {
    static_cast<std::string*>(destination)->append(contents, size * count);
    return size * count;
}

std::string escape(CURL* curl, const std::string& value) {
    std::unique_ptr<char, decltype(&curl_free)> encoded(curl_easy_escape(curl, value.c_str(), 0),
                                                        &curl_free);
    if (!encoded) throw std::runtime_error("could not URL-encode request");
    return encoded.get();
}

std::string join_symbols(CURL* curl, const std::vector<std::string>& symbols) {
    std::ostringstream joined;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        if (index) joined << ',';
        joined << symbols[index];
    }
    return escape(curl, joined.str());
}

std::string request_json(CURL* curl, curl_slist* headers, const std::string& url) {
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "options-backtester/0.1");
    CURLcode rc{};
    long status = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        body.clear();
        rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (rc == CURLE_OK && status != 429 && status < 500) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * (1 << attempt)));
    }
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("Alpaca request failed: ") + curl_easy_strerror(rc));
    if (status < 200 || status >= 300)
        throw std::runtime_error("Alpaca returned HTTP " + std::to_string(status) + ": " + body);
    return body;
}

std::unique_ptr<curl_slist, HeaderCleanup> make_headers(const Credentials& credentials) {
    curl_slist* value = nullptr;
    value = curl_slist_append(value, ("APCA-API-KEY-ID: " + credentials.api_key).c_str());
    value = curl_slist_append(value, ("APCA-API-SECRET-KEY: " + credentials.api_secret).c_str());
    return std::unique_ptr<curl_slist, HeaderCleanup>(value);
}

}  // namespace

AlpacaClient::AlpacaClient(Credentials credentials, std::string base_url,
                           std::string trading_base_url)
    : credentials_(std::move(credentials)), base_url_(std::move(base_url)),
      trading_base_url_(std::move(trading_base_url)) {
    if (credentials_.api_key.empty() || credentials_.api_secret.empty())
        throw std::invalid_argument("Alpaca credentials cannot be empty");
}

std::vector<data::Bar> AlpacaClient::fetch_bars(const BarsRequest& request) const {
    if (request.symbols.empty()) throw std::invalid_argument("at least one symbol is required");
    std::unique_ptr<CURL, CurlCleanup> curl(curl_easy_init());
    if (!curl) throw std::runtime_error("could not initialize libcurl");

    auto headers = make_headers(credentials_);

    std::vector<data::Bar> result;
    std::string page_token;
    do {
        std::ostringstream url;
        url << base_url_ << "/v2/stocks/bars?symbols=" << join_symbols(curl.get(), request.symbols)
            << "&start=" << escape(curl.get(), request.start)
            << "&end=" << escape(curl.get(), request.end)
            << "&timeframe=" << escape(curl.get(), request.timeframe)
            << "&adjustment=" << escape(curl.get(), request.adjustment)
            << "&feed=" << escape(curl.get(), request.feed) << "&limit=10000&sort=asc";
        if (!page_token.empty()) url << "&page_token=" << escape(curl.get(), page_token);

        auto page = parse_bars_page(request_json(curl.get(), headers.get(), url.str()));
        result.insert(result.end(), page.bars.begin(), page.bars.end());
        page_token = std::move(page.next_page_token);
    } while (!page_token.empty());
    return result;
}

std::vector<data::OptionContract> AlpacaClient::fetch_option_contracts(
    const OptionContractsRequest& request) const {
    if (request.underlying_symbols.empty()) throw std::invalid_argument("underlying symbol required");
    std::unique_ptr<CURL, CurlCleanup> curl(curl_easy_init());
    auto headers = make_headers(credentials_);
    std::vector<data::OptionContract> result;
    std::string page_token;
    do {
        std::ostringstream url;
        url << trading_base_url_ << "/v2/options/contracts?underlying_symbols="
            << join_symbols(curl.get(), request.underlying_symbols) << "&limit=10000&status="
            << escape(curl.get(), request.status);
        if (!request.expiration_date_gte.empty()) url << "&expiration_date_gte=" << escape(curl.get(), request.expiration_date_gte);
        if (!request.expiration_date_lte.empty()) url << "&expiration_date_lte=" << escape(curl.get(), request.expiration_date_lte);
        if (!page_token.empty()) url << "&page_token=" << escape(curl.get(), page_token);
        auto page = parse_option_contracts_page(request_json(curl.get(), headers.get(), url.str()));
        result.insert(result.end(), page.contracts.begin(), page.contracts.end());
        page_token = std::move(page.next_page_token);
    } while (!page_token.empty());
    return result;
}

std::vector<data::Bar> AlpacaClient::fetch_option_bars(const BarsRequest& request) const {
    if (request.symbols.empty()) throw std::invalid_argument("option symbols required");
    std::unique_ptr<CURL, CurlCleanup> curl(curl_easy_init());
    auto headers = make_headers(credentials_);
    std::vector<data::Bar> result;
    std::string page_token;
    do {
        std::ostringstream url;
        url << base_url_ << "/v1beta1/options/bars?symbols=" << join_symbols(curl.get(), request.symbols)
            << "&start=" << escape(curl.get(), request.start) << "&end=" << escape(curl.get(), request.end)
            << "&timeframe=" << escape(curl.get(), request.timeframe) << "&limit=10000&sort=asc";
        if (!page_token.empty()) url << "&page_token=" << escape(curl.get(), page_token);
        auto page = parse_bars_page(request_json(curl.get(), headers.get(), url.str()));
        result.insert(result.end(), page.bars.begin(), page.bars.end());
        page_token = std::move(page.next_page_token);
    } while (!page_token.empty());
    return result;
}

std::vector<data::OptionQuote> AlpacaClient::fetch_latest_option_quotes(
    const std::vector<std::string>& symbols, const std::string& feed) const {
    if (symbols.empty() || symbols.size() > 100) throw std::invalid_argument("1-100 option symbols required");
    std::unique_ptr<CURL, CurlCleanup> curl(curl_easy_init());
    auto headers = make_headers(credentials_);
    const auto url = base_url_ + "/v1beta1/options/quotes/latest?symbols=" +
                     join_symbols(curl.get(), symbols) + "&feed=" + escape(curl.get(), feed);
    return parse_option_quotes_page(request_json(curl.get(), headers.get(), url)).quotes;
}

std::vector<data::OptionSnapshot> AlpacaClient::fetch_option_chain(
    const OptionChainRequest& request) const {
    if (request.underlying_symbol.empty()) throw std::invalid_argument("underlying symbol required");
    std::unique_ptr<CURL, CurlCleanup> curl(curl_easy_init());
    auto headers = make_headers(credentials_);
    std::vector<data::OptionSnapshot> result;
    std::string page_token;
    const auto observed_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    do {
        std::ostringstream url;
        url << base_url_ << "/v1beta1/options/snapshots/" << escape(curl.get(), request.underlying_symbol)
            << "?feed=" << escape(curl.get(), request.feed) << "&limit=1000";
        if (!request.expiration_date_gte.empty()) url << "&expiration_date_gte=" << escape(curl.get(), request.expiration_date_gte);
        if (!request.expiration_date_lte.empty()) url << "&expiration_date_lte=" << escape(curl.get(), request.expiration_date_lte);
        if (!request.type.empty()) url << "&type=" << escape(curl.get(), request.type);
        if (!page_token.empty()) url << "&page_token=" << escape(curl.get(), page_token);
        auto page = parse_option_snapshots_page(request_json(curl.get(), headers.get(), url.str()), observed_at);
        result.insert(result.end(), page.snapshots.begin(), page.snapshots.end());
        page_token = std::move(page.next_page_token);
    } while (!page_token.empty());
    return result;
}

MarketClock AlpacaClient::fetch_market_clock() const {
    std::unique_ptr<CURL,CurlCleanup> curl(curl_easy_init());
    if(!curl) throw std::runtime_error("could not initialize libcurl");
    auto headers=make_headers(credentials_);
    const auto body=request_json(curl.get(),headers.get(),trading_base_url_+"/v2/clock");
    const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(body));
    if(!document.isObject()) throw std::runtime_error("invalid Alpaca market clock response");
    const auto object=document.object();
    return {object.value("timestamp").toString().toStdString(),object.value("is_open").toBool(),
            object.value("next_open").toString().toStdString(),
            object.value("next_close").toString().toStdString()};
}

}  // namespace options::providers::alpaca
