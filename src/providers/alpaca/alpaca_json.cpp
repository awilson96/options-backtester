#include "options/providers/alpaca/alpaca_json.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <stdexcept>

namespace options::providers::alpaca {
namespace {

std::string required_string(const QJsonObject& object, const char* key) {
    const auto value = object.value(key);
    if (!value.isString()) throw std::runtime_error(std::string("missing string field: ") + key);
    return value.toString().toStdString();
}

double required_number(const QJsonObject& object, const char* key) {
    const auto value = object.value(key);
    if (!value.isDouble()) throw std::runtime_error(std::string("missing number field: ") + key);
    return value.toDouble();
}

}  // namespace

BarsPage parse_bars_page(std::string_view json) {
    QJsonParseError parse_error;
    const auto bytes = QByteArray::fromRawData(json.data(), static_cast<qsizetype>(json.size()));
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("invalid Alpaca JSON: " + parse_error.errorString().toStdString());
    }

    BarsPage page;
    const auto root = document.object();
    const auto bars_by_symbol = root.value("bars");
    if (!bars_by_symbol.isObject()) throw std::runtime_error("Alpaca response has no bars object");

    const auto bars_object = bars_by_symbol.toObject();
    for (auto symbol_it = bars_object.constBegin(); symbol_it != bars_object.constEnd(); ++symbol_it) {
        if (!symbol_it.value().isArray()) throw std::runtime_error("bars value is not an array");
        for (const auto& value : symbol_it.value().toArray()) {
            if (!value.isObject()) throw std::runtime_error("bar is not an object");
            const auto bar = value.toObject();
            page.bars.push_back({
                symbol_it.key().toStdString(), required_string(bar, "t"),
                required_number(bar, "o"), required_number(bar, "h"),
                required_number(bar, "l"), required_number(bar, "c"),
                static_cast<std::int64_t>(required_number(bar, "v")),
                static_cast<std::int64_t>(required_number(bar, "n")),
                required_number(bar, "vw")});
        }
    }
    const auto token = root.value("next_page_token");
    if (token.isString()) page.next_page_token = token.toString().toStdString();
    return page;
}

}  // namespace options::providers::alpaca
