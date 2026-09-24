#pragma once

#include "calculator.h"

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

// Offline unit converter (US-047): "5 km in mi", "72 f to c", "3 GB in GiB".
// Runs only after the calculator found nothing. Pure logic, no window or
// locale state: the caller passes Windows' decimal separator. Fully offline,
// so no currency.
namespace leanlauncher {
namespace convert {

enum class Dimension { Length, Mass, Volume, Temperature, Data, Speed, Area, Time };

struct Unit {
    const wchar_t* symbol;   // shown in results
    Dimension dimension;
    double factor;           // to the dimension's base unit (m, kg, L, K, byte, m/s, m2, s)
    double offset;           // temperature only: base = value * factor + offset
    const wchar_t* aliases;  // '|'-separated, lowercase
    const wchar_t* note;     // explains an ambiguous reading, or nullptr
};

inline constexpr wchar_t kUsVolumeNote[] = L"US units - type imp gal, imp pt, or imp fl oz for imperial";
inline constexpr wchar_t kDataNote[] = L"KB = 1000 bytes - type KiB for 1024";

inline constexpr Unit kUnits[] = {
    // Length (base: metre)
    {L"m", Dimension::Length, 1.0, 0, L"m|metre|metres|meter|meters", nullptr},
    {L"km", Dimension::Length, 1000.0, 0, L"km|kilometre|kilometres|kilometer|kilometers", nullptr},
    {L"cm", Dimension::Length, 0.01, 0, L"cm|centimetre|centimetres|centimeter|centimeters", nullptr},
    {L"mm", Dimension::Length, 0.001, 0, L"mm|millimetre|millimetres|millimeter|millimeters", nullptr},
    {L"mi", Dimension::Length, 1609.344, 0, L"mi|mile|miles", nullptr},
    {L"yd", Dimension::Length, 0.9144, 0, L"yd|yard|yards", nullptr},
    {L"ft", Dimension::Length, 0.3048, 0, L"ft|foot|feet", nullptr},
    {L"in", Dimension::Length, 0.0254, 0, L"in|inch|inches", nullptr},
    {L"nmi", Dimension::Length, 1852.0, 0, L"nmi|nautical mile|nautical miles", nullptr},
    // Mass (base: kilogram)
    {L"kg", Dimension::Mass, 1.0, 0, L"kg|kilogram|kilograms|kilo|kilos", nullptr},
    {L"g", Dimension::Mass, 0.001, 0, L"g|gram|grams", nullptr},
    {L"mg", Dimension::Mass, 0.000001, 0, L"mg|milligram|milligrams", nullptr},
    {L"t", Dimension::Mass, 1000.0, 0, L"t|tonne|tonnes|metric ton|metric tons", nullptr},
    {L"lb", Dimension::Mass, 0.45359237, 0, L"lb|lbs|pound|pounds", nullptr},
    {L"oz", Dimension::Mass, 0.028349523125, 0, L"oz|ounce|ounces", nullptr},
    {L"st", Dimension::Mass, 6.35029318, 0, L"st|stone|stones", nullptr},
    // Volume (base: litre)
    {L"L", Dimension::Volume, 1.0, 0, L"l|litre|litres|liter|liters", nullptr},
    {L"mL", Dimension::Volume, 0.001, 0, L"ml|millilitre|millilitres|milliliter|milliliters", nullptr},
    {L"cL", Dimension::Volume, 0.01, 0, L"cl|centilitre|centilitres|centiliter|centiliters", nullptr},
    {L"dL", Dimension::Volume, 0.1, 0, L"dl|decilitre|decilitres|deciliter|deciliters", nullptr},
    {L"m³", Dimension::Volume, 1000.0, 0, L"m3|m^3|cubic metre|cubic metres|cubic meter|cubic meters", nullptr},
    {L"gal", Dimension::Volume, 3.785411784, 0, L"gal|gallon|gallons|us gal", kUsVolumeNote},
    {L"imp gal", Dimension::Volume, 4.54609, 0, L"imp gal|imperial gallon|imperial gallons|uk gal", nullptr},
    {L"qt", Dimension::Volume, 0.946352946, 0, L"qt|quart|quarts", kUsVolumeNote},
    {L"pt", Dimension::Volume, 0.473176473, 0, L"pt|pint|pints", kUsVolumeNote},
    {L"imp pt", Dimension::Volume, 0.56826125, 0, L"imp pt|imperial pint|imperial pints|uk pt", nullptr},
    {L"cup", Dimension::Volume, 0.2365882365, 0, L"cup|cups", kUsVolumeNote},
    {L"fl oz", Dimension::Volume, 0.0295735295625, 0, L"fl oz|floz|fluid ounce|fluid ounces", kUsVolumeNote},
    {L"imp fl oz", Dimension::Volume, 0.0284130625, 0, L"imp fl oz|imperial fluid ounce|imperial fluid ounces", nullptr},
    {L"tbsp", Dimension::Volume, 0.01478676478125, 0, L"tbsp|tablespoon|tablespoons", kUsVolumeNote},
    {L"tsp", Dimension::Volume, 0.00492892159375, 0, L"tsp|teaspoon|teaspoons", kUsVolumeNote},
    // Temperature (base: kelvin)
    {L"°C", Dimension::Temperature, 1.0, 273.15, L"c|°c|celsius|centigrade", nullptr},
    {L"°F", Dimension::Temperature, 5.0 / 9.0, 273.15 - 32.0 * 5.0 / 9.0, L"f|°f|fahrenheit", nullptr},
    {L"K", Dimension::Temperature, 1.0, 0, L"k|kelvin", nullptr},
    // Data (base: byte)
    {L"B", Dimension::Data, 1.0, 0, L"b|byte|bytes", nullptr},
    {L"bit", Dimension::Data, 0.125, 0, L"bit|bits", nullptr},
    {L"KB", Dimension::Data, 1e3, 0, L"kb|kilobyte|kilobytes", kDataNote},
    {L"MB", Dimension::Data, 1e6, 0, L"mb|megabyte|megabytes", kDataNote},
    {L"GB", Dimension::Data, 1e9, 0, L"gb|gigabyte|gigabytes", kDataNote},
    {L"TB", Dimension::Data, 1e12, 0, L"tb|terabyte|terabytes", kDataNote},
    {L"KiB", Dimension::Data, 1024.0, 0, L"kib|kibibyte|kibibytes", kDataNote},
    {L"MiB", Dimension::Data, 1048576.0, 0, L"mib|mebibyte|mebibytes", kDataNote},
    {L"GiB", Dimension::Data, 1073741824.0, 0, L"gib|gibibyte|gibibytes", kDataNote},
    {L"TiB", Dimension::Data, 1099511627776.0, 0, L"tib|tebibyte|tebibytes", kDataNote},
    // Speed (base: metre per second)
    {L"m/s", Dimension::Speed, 1.0, 0, L"m/s|mps|metres per second|meters per second", nullptr},
    {L"km/h", Dimension::Speed, 1000.0 / 3600.0, 0, L"km/h|kmh|kph|kmph", nullptr},
    {L"mph", Dimension::Speed, 1609.344 / 3600.0, 0, L"mph|miles per hour", nullptr},
    {L"kn", Dimension::Speed, 1852.0 / 3600.0, 0, L"kn|knot|knots", nullptr},
    {L"ft/s", Dimension::Speed, 0.3048, 0, L"ft/s|fps", nullptr},
    // Area (base: square metre)
    {L"m²", Dimension::Area, 1.0, 0, L"m2|m^2|m²|sqm|sq m|square metre|square metres|square meter|square meters", nullptr},
    {L"km²", Dimension::Area, 1e6, 0, L"km2|km^2|km²|sq km|square kilometre|square kilometres|square kilometer|square kilometers", nullptr},
    {L"cm²", Dimension::Area, 1e-4, 0, L"cm2|cm^2|cm²|sq cm", nullptr},
    {L"ha", Dimension::Area, 1e4, 0, L"ha|hectare|hectares", nullptr},
    {L"acre", Dimension::Area, 4046.8564224, 0, L"acre|acres|ac", nullptr},
    {L"ft²", Dimension::Area, 0.09290304, 0, L"ft2|ft^2|ft²|sqft|sq ft|square foot|square feet", nullptr},
    {L"mi²", Dimension::Area, 2589988.110336, 0, L"mi2|mi^2|mi²|sq mi|square mile|square miles", nullptr},
    // Time (base: second)
    {L"s", Dimension::Time, 1.0, 0, L"s|sec|secs|second|seconds", nullptr},
    {L"ms", Dimension::Time, 0.001, 0, L"ms|millisecond|milliseconds", nullptr},
    {L"min", Dimension::Time, 60.0, 0, L"min|mins|minute|minutes", nullptr},
    {L"h", Dimension::Time, 3600.0, 0, L"h|hr|hrs|hour|hours", nullptr},
    {L"d", Dimension::Time, 86400.0, 0, L"d|day|days", nullptr},
    {L"wk", Dimension::Time, 604800.0, 0, L"wk|week|weeks", nullptr},
    {L"yr", Dimension::Time, 31557600.0, 0, L"yr|yrs|year|years", nullptr},
};

// Targets a bare "<number> <unit>" lists, per dimension (the source unit is skipped).
inline constexpr const wchar_t* kCommonTargets[][6] = {
    {L"mi", L"km", L"m", L"ft", L"in", L"yd"},          // Length
    {L"lb", L"kg", L"g", L"oz", nullptr, nullptr},       // Mass
    {L"L", L"gal", L"cup", L"mL", nullptr, nullptr},     // Volume
    {L"°C", L"°F", L"K", nullptr, nullptr, nullptr},  // Temperature
    {L"MB", L"MiB", L"GB", L"GiB", nullptr, nullptr},    // Data
    {L"km/h", L"mph", L"m/s", L"kn", nullptr, nullptr},  // Speed
    {L"m²", L"ft²", L"acre", L"ha", nullptr, nullptr},  // Area
    {L"min", L"h", L"s", L"d", nullptr, nullptr},        // Time
};

struct ConversionRow {
    std::wstring text;       // "3.107 mi" - the row's answer
    std::wstring valueText;  // "3.107" - what Enter copies
    std::wstring copyText;   // "5 km = 3.107 mi" - the second copy action
    std::wstring source;     // "5 km"
    std::wstring note;       // "Conversion", or an explanation of an ambiguous unit
};

namespace detail {

inline std::wstring Lower(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    bool lastSpace = false;
    for (wchar_t ch : s) {
        if (iswspace(ch)) {
            if (!out.empty() && !lastSpace) out += L' ';
            lastSpace = true;
            continue;
        }
        out += static_cast<wchar_t>(towlower(ch));
        lastSpace = false;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

inline const Unit* FindUnit(std::wstring_view lowered) {
    if (lowered.empty()) return nullptr;
    for (const auto& unit : kUnits) {
        std::wstring_view aliases(unit.aliases);
        size_t start = 0;
        while (start <= aliases.size()) {
            size_t bar = aliases.find(L'|', start);
            if (bar == std::wstring_view::npos) bar = aliases.size();
            if (aliases.substr(start, bar - start) == lowered) return &unit;
            start = bar + 1;
        }
    }
    return nullptr;
}

inline const Unit* FindBySymbol(const wchar_t* symbol) {
    for (const auto& unit : kUnits) {
        if (wcscmp(unit.symbol, symbol) == 0) return &unit;
    }
    return nullptr;
}

// Splits "<number part> <unit>" by the longest unit alias at the end.
inline bool SplitValueAndUnit(std::wstring_view lowered, std::wstring& number, const Unit*& unit, size_t& aliasLength) {
    unit = nullptr;
    for (const auto& candidate : kUnits) {
        std::wstring_view aliases(candidate.aliases);
        size_t start = 0;
        while (start <= aliases.size()) {
            size_t bar = aliases.find(L'|', start);
            if (bar == std::wstring_view::npos) bar = aliases.size();
            const std::wstring_view alias = aliases.substr(start, bar - start);
            start = bar + 1;
            if (alias.empty() || alias.size() >= lowered.size() || alias.size() <= aliasLength) continue;
            if (lowered.substr(lowered.size() - alias.size()) != alias) continue;
            const wchar_t before = lowered[lowered.size() - alias.size() - 1];
            if (!(before == L' ' || iswdigit(before) || before == L')' || before == L'.')) continue;
            unit = &candidate;
            aliasLength = alias.size();
        }
    }
    if (!unit) return false;
    std::wstring_view rest = lowered.substr(0, lowered.size() - aliasLength);
    while (!rest.empty() && rest.back() == L' ') rest.remove_suffix(1);
    number.assign(rest);
    return !number.empty();
}

inline bool ParseNumber(std::wstring number, wchar_t decimalSeparator, double& out, bool& plain) {
    if (decimalSeparator == L',' && number.find(L'.') == std::wstring::npos) {
        for (auto& ch : number) {
            if (ch == L',') ch = L'.';
        }
    }
    wchar_t* end = nullptr;
    const double value = std::wcstod(number.c_str(), &end);
    if (end && *end == 0 && end != number.c_str()) {
        out = value;
        plain = true;
        return std::isfinite(value);
    }
    plain = false;
    const auto calc = takeoff::EvaluateExpression(number);
    if (!calc) return false;
    out = calc->value;
    return std::isfinite(out);
}

inline std::wstring FormatValue(double value) {
    if (!std::isfinite(value)) return L"";
    const double magnitude = std::fabs(value);
    wchar_t buffer[64];
    const double rounded = std::round(value);
    if (magnitude < 1e15 && std::fabs(value - rounded) < 1e-9 * (std::max)(1.0, magnitude)) {
        swprintf(buffer, std::size(buffer), L"%.0f", rounded == 0 ? 0.0 : rounded);
        return buffer;
    }
    for (int digits = 1; digits <= 5; ++digits) {
        swprintf(buffer, std::size(buffer), L"%.*g", digits, value);
        if (std::fabs(std::wcstod(buffer, nullptr) - value) <= 1e-12 * (std::max)(1.0, magnitude)) return buffer;
    }
    if (magnitude < 1e-4 || magnitude >= 1e15) {
        swprintf(buffer, std::size(buffer), L"%.4g", value);
        return buffer;
    }
    const int before = static_cast<int>(std::floor(std::log10(magnitude))) + 1;
    const int decimals = (std::max)(0, 4 - before);
    swprintf(buffer, std::size(buffer), L"%.*f", decimals, value);
    std::wstring s(buffer);
    if (s.find(L'.') != std::wstring::npos) {
        while (s.back() == L'0') s.pop_back();
        if (s.back() == L'.') s.pop_back();
    }
    return s;
}

inline ConversionRow MakeRow(double value, const Unit& from, const Unit& to) {
    const double base = value * from.factor + from.offset;
    const double converted = (base - to.offset) / to.factor;
    ConversionRow row;
    row.valueText = FormatValue(converted);
    row.text = row.valueText + L" " + to.symbol;
    row.source = FormatValue(value) + L" " + from.symbol;
    row.copyText = row.source + L" = " + row.text;
    const wchar_t* note = from.note ? from.note : to.note;
    row.note = note ? std::wstring(L"Conversion - ") + note : L"Conversion";
    return row;
}

// Finds the separator between source and target: "->", " to ", "=", or the
// last " in " (so "10 in in cm" splits after the unit "in").
inline bool SplitQuery(const std::wstring& lowered, std::wstring& left, std::wstring& right) {
    size_t pos = lowered.find(L"->");
    size_t len = 2;
    if (pos == std::wstring::npos) { pos = lowered.find(L" to "); len = 4; }
    if (pos == std::wstring::npos) { pos = lowered.find(L'='); len = 1; }
    if (pos == std::wstring::npos) { pos = lowered.rfind(L" in "); len = 4; }
    if (pos == std::wstring::npos) return false;
    left = Lower(lowered.substr(0, pos));
    right = Lower(lowered.substr(pos + len));
    return !left.empty() && !right.empty();
}

} // namespace detail

// Empty when the query isn't a conversion. Otherwise one row for
// "<value> <unit> in|to|->|= <unit>", or up to 4 rows for a bare
// "<number> <unit>" with an unambiguous (2+ letter) unit.
inline std::vector<ConversionRow> EvaluateConversion(std::wstring_view query, wchar_t decimalSeparator) {
    std::vector<ConversionRow> rows;
    const std::wstring lowered = detail::Lower(query);
    bool hasDigit = false;
    for (wchar_t ch : lowered) hasDigit = hasDigit || iswdigit(ch);
    if (!hasDigit) return rows;

    std::wstring left, right;
    if (detail::SplitQuery(lowered, left, right)) {
        std::wstring number;
        const Unit* from = nullptr;
        size_t aliasLength = 0;
        const Unit* to = detail::FindUnit(right);
        if (!to || !detail::SplitValueAndUnit(left, number, from, aliasLength)) return rows;
        if (from->dimension != to->dimension) return rows;
        double value = 0;
        bool plain = false;
        if (!detail::ParseNumber(number, decimalSeparator, value, plain)) return rows;
        rows.push_back(detail::MakeRow(value, *from, *to));
        return rows;
    }

    // Bare "<number> <unit>": only plain numbers and units of 2+ letters
    // ("in" and single letters are too easily ordinary words).
    std::wstring number;
    const Unit* from = nullptr;
    size_t aliasLength = 0;
    if (!detail::SplitValueAndUnit(lowered, number, from, aliasLength)) return rows;
    if (aliasLength < 2 || lowered.substr(lowered.size() - aliasLength) == L"in") return rows;
    double value = 0;
    bool plain = false;
    if (!detail::ParseNumber(number, decimalSeparator, value, plain) || !plain) return rows;
    for (const wchar_t* symbol : kCommonTargets[static_cast<int>(from->dimension)]) {
        if (!symbol || rows.size() >= 4) break;
        const Unit* to = detail::FindBySymbol(symbol);
        if (!to || to == from) continue;
        rows.push_back(detail::MakeRow(value, *from, *to));
    }
    return rows;
}

} // namespace convert
} // namespace leanlauncher
