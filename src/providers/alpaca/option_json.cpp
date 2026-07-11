#include "options/providers/alpaca/option_json.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <stdexcept>

namespace options::providers::alpaca {
namespace {

QJsonObject root_object(std::string_view json) {
    QJsonParseError error;
    const auto bytes = QByteArray::fromRawData(json.data(), static_cast<qsizetype>(json.size()));
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("invalid Alpaca option JSON: " + error.errorString().toStdString());
    return document.object();
}

std::string text(const QJsonObject& value, const char* key) {
    return value.value(key).toString().toStdString();
}

std::optional<double> number(const QJsonObject& value, const char* key) {
    const auto field = value.value(key);
    if (field.isDouble()) return field.toDouble();
    if (field.isString()) {
        bool ok = false;
        const auto result = field.toString().toDouble(&ok);
        if (ok) return result;
    }
    return std::nullopt;
}

std::optional<std::int64_t> integer(const QJsonObject& value, const char* key) {
    const auto parsed = number(value, key);
    if (!parsed) return std::nullopt;
    return static_cast<std::int64_t>(*parsed);
}

data::OptionQuote quote(const std::string& symbol, const QJsonObject& value) {
    return {symbol, text(value, "t"), number(value, "bp").value_or(0.0),
            number(value, "ap").value_or(0.0), integer(value, "bs").value_or(0),
            integer(value, "as").value_or(0), text(value, "bx"), text(value, "ax")};
}

std::string token(const QJsonObject& root) {
    return root.value("next_page_token").toString().toStdString();
}

}  // namespace

ContractsPage parse_option_contracts_page(std::string_view json) {
    const auto root = root_object(json);
    const auto values = root.value("option_contracts");
    if (!values.isArray()) throw std::runtime_error("option_contracts is not an array");
    ContractsPage page;
    page.next_page_token = token(root);
    for (const auto& item : values.toArray()) {
        if (!item.isObject()) throw std::runtime_error("option contract is not an object");
        const auto value = item.toObject();
        data::OptionContract contract;
        contract.id = text(value, "id");
        contract.symbol = text(value, "symbol");
        contract.name = text(value, "name");
        contract.underlying_symbol = text(value, "underlying_symbol");
        contract.root_symbol = text(value, "root_symbol");
        contract.expiration_date = text(value, "expiration_date");
        contract.type = text(value, "type");
        contract.style = text(value, "style");
        contract.status = text(value, "status");
        contract.strike_price = number(value, "strike_price").value_or(0.0);
        contract.multiplier = integer(value, "size").value_or(100);
        contract.tradable = value.value("tradable").toBool();
        contract.open_interest = integer(value, "open_interest");
        contract.open_interest_date = text(value, "open_interest_date");
        contract.close_price = number(value, "close_price");
        contract.close_price_date = text(value, "close_price_date");
        if (contract.symbol.empty()) throw std::runtime_error("option contract has no symbol");
        page.contracts.push_back(std::move(contract));
    }
    return page;
}

QuotesPage parse_option_quotes_page(std::string_view json) {
    const auto root = root_object(json);
    const auto values = root.value("quotes");
    if (!values.isObject()) throw std::runtime_error("quotes is not an object");
    QuotesPage page;
    page.next_page_token = token(root);
    const auto object = values.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isObject()) page.quotes.push_back(quote(it.key().toStdString(), it.value().toObject()));
    }
    return page;
}

SnapshotsPage parse_option_snapshots_page(std::string_view json, const std::string& observed_at) {
    const auto root = root_object(json);
    const auto values = root.value("snapshots");
    if (!values.isObject()) throw std::runtime_error("snapshots is not an object");
    SnapshotsPage page;
    page.next_page_token = token(root);
    const auto object = values.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const auto value = it.value().toObject();
        data::OptionSnapshot snapshot;
        snapshot.symbol = it.key().toStdString();
        snapshot.observed_at = observed_at;
        if (value.value("latestQuote").isObject())
            snapshot.latest_quote = quote(snapshot.symbol, value.value("latestQuote").toObject());
        if (value.value("latestTrade").isObject()) {
            const auto trade = value.value("latestTrade").toObject();
            snapshot.trade_timestamp = text(trade, "t");
            snapshot.trade_price = number(trade, "p");
            snapshot.trade_size = integer(trade, "s");
        }
        snapshot.implied_volatility = number(value, "impliedVolatility");
        if (value.value("greeks").isObject()) {
            const auto greeks = value.value("greeks").toObject();
            snapshot.delta = number(greeks, "delta");
            snapshot.gamma = number(greeks, "gamma");
            snapshot.theta = number(greeks, "theta");
            snapshot.vega = number(greeks, "vega");
            snapshot.rho = number(greeks, "rho");
        }
        page.snapshots.push_back(std::move(snapshot));
    }
    return page;
}

}  // namespace options::providers::alpaca
