#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takeoff {

struct CalculationResult {
    std::wstring expression;
    std::wstring rawResult;
    std::wstring formattedResult;
    double value = 0.0;
};

namespace detail {

constexpr double kPi  = 3.14159265358979323846;
constexpr double kE   = 2.71828182845904523536;
constexpr double kTau = 6.28318530717958647692;
constexpr double kPhi = 1.61803398874989484820;

enum class TokenType {
    Number,
    Identifier,
    Plus,
    Minus,
    Multiply,
    Divide,
    Modulo,
    Power,
    Factorial,
    Percent,
    LParen,
    RParen,
    Comma,
    End
};

struct Token {
    TokenType type = TokenType::End;
    double numberValue = 0.0;
    std::wstring stringValue;
};

inline bool IsWhitespace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

inline double Factorial(double n) {
    if (n < 0.0 || n > 170.0) return std::numeric_limits<double>::quiet_NaN();
    double r = std::round(n);
    if (std::abs(n - r) > 1e-9) return std::numeric_limits<double>::quiet_NaN();
    int k = static_cast<int>(r);
    double result = 1.0;
    for (int i = 2; i <= k; ++i) {
        result *= i;
    }
    return result;
}

inline std::wstring FormatNumber(double value) {
    if (std::isnan(value) || std::isinf(value)) return L"";

    // Clean up floating point noise near integers
    double rounded = std::round(value);
    if (std::abs(value - rounded) < 1e-11) {
        value = rounded;
    }

    if (std::abs(value) >= 1e15 || (std::abs(value) < 1e-6 && value != 0.0)) {
        wchar_t buffer[64];
        swprintf_s(buffer, L"%.8g", value);
        return std::wstring(buffer);
    }

    wchar_t buffer[64];
    swprintf_s(buffer, L"%.10f", value);
    std::wstring s(buffer);

    // Strip trailing zeroes after decimal point
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) {
        while (s.size() > dot && s.back() == L'0') {
            s.pop_back();
        }
        if (s.back() == L'.') {
            s.pop_back();
        }
    }

    return s;
}

class Lexer {
public:
    explicit Lexer(std::wstring_view input) : text_(input), pos_(0) {}

    bool Tokenize(std::vector<Token>& tokens, bool& hasMathIndicator, bool& hasDigitOrConst) {
        tokens.clear();
        hasMathIndicator = false;
        hasDigitOrConst = false;

        // Strip leading '=' if present
        size_t start = 0;
        while (start < text_.size() && IsWhitespace(text_[start])) ++start;
        if (start < text_.size() && text_[start] == L'=') {
            hasMathIndicator = true;
            pos_ = start + 1;
        } else {
            pos_ = 0;
        }

        // Strip trailing '=' if present
        size_t end = text_.size();
        while (end > pos_ && IsWhitespace(text_[end - 1])) --end;
        if (end > pos_ && text_[end - 1] == L'=') {
            hasMathIndicator = true;
            --end;
        }
        std::wstring_view sub = text_.substr(pos_, end - pos_);
        pos_ = 0;
        text_ = sub;

        while (pos_ < text_.size()) {
            wchar_t ch = text_[pos_];
            if (IsWhitespace(ch)) {
                ++pos_;
                continue;
            }

            // Digit or decimal point starting a number
            if (iswdigit(ch) || (ch == L'.' && pos_ + 1 < text_.size() && iswdigit(text_[pos_ + 1]))) {
                hasDigitOrConst = true;
                size_t numStart = pos_;
                bool hasDot = false;
                while (pos_ < text_.size()) {
                    wchar_t c = text_[pos_];
                    if (iswdigit(c)) {
                        ++pos_;
                    } else if (c == L'.' && !hasDot) {
                        hasDot = true;
                        ++pos_;
                    } else if (c == L',' && pos_ + 1 < text_.size() && iswdigit(text_[pos_ + 1])) {
                        // Skip thousands separator comma
                        ++pos_;
                    } else {
                        break;
                    }
                }
                // Check scientific notation exponent (e.g. 1e5, 2.5e-3)
                if (pos_ < text_.size() && (text_[pos_] == L'e' || text_[pos_] == L'E')) {
                    size_t ePos = pos_;
                    ++pos_;
                    if (pos_ < text_.size() && (text_[pos_] == L'+' || text_[pos_] == L'-')) {
                        ++pos_;
                    }
                    if (pos_ < text_.size() && iswdigit(text_[pos_])) {
                        while (pos_ < text_.size() && iswdigit(text_[pos_])) ++pos_;
                    } else {
                        pos_ = ePos; // not scientific notation
                    }
                }

                std::wstring numStr;
                for (size_t i = numStart; i < pos_; ++i) {
                    if (text_[i] != L',') numStr.push_back(text_[i]);
                }
                wchar_t* endPtr = nullptr;
                double val = wcstod(numStr.c_str(), &endPtr);
                if (endPtr == numStr.c_str()) return false;
                tokens.push_back({TokenType::Number, val, numStr});
                continue;
            }

            // 'x' or 'X' as multiplication operator when preceded by an operand (e.g. 10x10, 10 x 10, 5x(2+3))
            if (ch == L'x' || ch == L'X') {
                if (!tokens.empty() &&
                    (tokens.back().type == TokenType::Number ||
                     tokens.back().type == TokenType::RParen ||
                     tokens.back().type == TokenType::Factorial ||
                     tokens.back().type == TokenType::Percent)) {
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Multiply, 0.0, L"*"});
                    ++pos_;
                    continue;
                }
            }

            // Words: functions or constants
            if (iswalpha(ch) || ch == L'π' || ch == L'τ' || ch == L'ϕ') {
                size_t idStart = pos_;
                while (pos_ < text_.size() && (iswalnum(text_[pos_]) || text_[pos_] == L'_')) {
                    ++pos_;
                }
                if (pos_ == idStart) {
                    // Unicode symbol like π
                    ++pos_;
                }
                std::wstring id(text_.substr(idStart, pos_ - idStart));
                std::wstring lowerId;
                lowerId.reserve(id.size());
                for (wchar_t c : id) lowerId.push_back(static_cast<wchar_t>(towlower(c)));

                // Constants
                if (lowerId == L"pi" || lowerId == L"π") {
                    hasDigitOrConst = true;
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Number, kPi, L"pi"});
                    continue;
                }
                if (lowerId == L"e") {
                    hasDigitOrConst = true;
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Number, kE, L"e"});
                    continue;
                }
                if (lowerId == L"tau" || lowerId == L"τ") {
                    hasDigitOrConst = true;
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Number, kTau, L"tau"});
                    continue;
                }
                if (lowerId == L"phi" || lowerId == L"ϕ") {
                    hasDigitOrConst = true;
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Number, kPhi, L"phi"});
                    continue;
                }

                // Known math functions
                if (lowerId == L"sqrt" || lowerId == L"cbrt" || lowerId == L"abs" ||
                    lowerId == L"fabs" || lowerId == L"sin" || lowerId == L"cos" ||
                    lowerId == L"tan" || lowerId == L"asin" || lowerId == L"acos" ||
                    lowerId == L"atan" || lowerId == L"atan2" || lowerId == L"sinh" ||
                    lowerId == L"cosh" || lowerId == L"tanh" || lowerId == L"ln" ||
                    lowerId == L"log" || lowerId == L"log10" || lowerId == L"log2" ||
                    lowerId == L"exp" || lowerId == L"ceil" || lowerId == L"floor" ||
                    lowerId == L"round" || lowerId == L"hypot" || lowerId == L"pow") {
                    hasMathIndicator = true;
                    tokens.push_back({TokenType::Identifier, 0.0, lowerId});
                    continue;
                }

                // Any unknown identifier means this is NOT a math expression (e.g. app search)
                return false;
            }

            // Operators
            hasMathIndicator = true;
            if (ch == L'+') {
                tokens.push_back({TokenType::Plus, 0.0, L"+"});
                ++pos_;
            } else if (ch == L'-') {
                tokens.push_back({TokenType::Minus, 0.0, L"-"});
                ++pos_;
            } else if (ch == L'*') {
                if (pos_ + 1 < text_.size() && text_[pos_ + 1] == L'*') {
                    tokens.push_back({TokenType::Power, 0.0, L"^"});
                    pos_ += 2;
                } else {
                    tokens.push_back({TokenType::Multiply, 0.0, L"*"});
                    ++pos_;
                }
            } else if (ch == L'/' || ch == L'÷') {
                tokens.push_back({TokenType::Divide, 0.0, L"/"});
                ++pos_;
            } else if (ch == L'×' || ch == L'·') {
                tokens.push_back({TokenType::Multiply, 0.0, L"*"});
                ++pos_;
            } else if (ch == L'%') {
                tokens.push_back({TokenType::Percent, 0.0, L"%"});
                ++pos_;
            } else if (ch == L'^') {
                tokens.push_back({TokenType::Power, 0.0, L"^"});
                ++pos_;
            } else if (ch == L'!') {
                tokens.push_back({TokenType::Factorial, 0.0, L"!"});
                ++pos_;
            } else if (ch == L'(') {
                tokens.push_back({TokenType::LParen, 0.0, L"("});
                ++pos_;
            } else if (ch == L')') {
                tokens.push_back({TokenType::RParen, 0.0, L")"});
                ++pos_;
            } else if (ch == L',') {
                tokens.push_back({TokenType::Comma, 0.0, L","});
                ++pos_;
            } else {
                // Any unrecognized character means this is NOT a calculation
                return false;
            }
        }

        tokens.push_back({TokenType::End, 0.0, L""});
        return true;
    }

private:
    std::wstring_view text_;
    size_t pos_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)), cursor_(0) {}

    bool Parse(double& result) {
        if (tokens_.empty() || tokens_[0].type == TokenType::End) return false;
        if (!Expression(result)) return false;
        return Current().type == TokenType::End;
    }

private:
    const Token& Current() const {
        return (cursor_ < tokens_.size()) ? tokens_[cursor_] : tokens_.back();
    }

    const Token& Advance() {
        if (cursor_ < tokens_.size()) ++cursor_;
        return Current();
    }

    bool Match(TokenType type) {
        if (Current().type == type) {
            Advance();
            return true;
        }
        return false;
    }

    bool Expression(double& result) {
        return AddSub(result);
    }

    bool AddSub(double& result) {
        if (!MulDiv(result)) return false;
        while (Current().type == TokenType::Plus || Current().type == TokenType::Minus) {
            TokenType op = Current().type;
            Advance();
            double right = 0.0;
            if (!MulDiv(right)) return false;
            if (op == TokenType::Plus) {
                result += right;
            } else {
                result -= right;
            }
        }
        return true;
    }

    bool MulDiv(double& result) {
        if (!Power(result)) return false;
        while (true) {
            TokenType op = Current().type;
            if (op == TokenType::Multiply || op == TokenType::Divide) {
                Advance();
                double right = 0.0;
                if (!Power(right)) return false;
                if (op == TokenType::Multiply) {
                    result *= right;
                } else {
                    if (right == 0.0) return false; // Division by zero
                    result /= right;
                }
            } else if (op == TokenType::Percent) {
                // Check if '%' is binary modulo: followed by a number or expression
                size_t saved = cursor_;
                Advance();
                double right = 0.0;
                if (Power(right)) {
                    // Binary modulo
                    if (right == 0.0) return false;
                    result = std::fmod(result, right);
                } else {
                    // Postfix percent: result / 100
                    cursor_ = saved + 1;
                    result /= 100.0;
                }
            } else if (Current().type == TokenType::LParen ||
                       Current().type == TokenType::Number ||
                       Current().type == TokenType::Identifier) {
                // Implicit multiplication: e.g. 2(3), (2+3)(4+5), 2pi
                double right = 0.0;
                if (!Power(right)) return false;
                result *= right;
            } else {
                break;
            }
        }
        return true;
    }

    bool Power(double& result) {
        if (!Postfix(result)) return false;
        if (Match(TokenType::Power)) {
            double right = 0.0;
            // Right-associative: 2^3^2 = 2^(3^2) = 512
            if (!Power(right)) return false;
            result = std::pow(result, right);
            if (std::isnan(result) || std::isinf(result)) return false;
        }
        return true;
    }

    bool Postfix(double& result) {
        if (!Factor(result)) return false;
        while (true) {
            if (Match(TokenType::Factorial)) {
                result = Factorial(result);
                if (std::isnan(result)) return false;
            } else if (Current().type == TokenType::Percent) {
                // If followed by an operator, ')', or end: treat as postfix percent
                if (cursor_ + 1 < tokens_.size()) {
                    TokenType next = tokens_[cursor_ + 1].type;
                    if (next == TokenType::Plus || next == TokenType::Minus ||
                        next == TokenType::Multiply || next == TokenType::Divide ||
                        next == TokenType::RParen || next == TokenType::End) {
                        Advance();
                        result /= 100.0;
                        continue;
                    }
                }
                break;
            } else {
                break;
            }
        }
        return true;
    }

    bool Factor(double& result) {
        if (Match(TokenType::Plus)) {
            return Factor(result);
        }
        if (Match(TokenType::Minus)) {
            if (!Factor(result)) return false;
            result = -result;
            return true;
        }
        return Primary(result);
    }

    bool Primary(double& result) {
        const Token& tok = Current();
        if (tok.type == TokenType::Number) {
            result = tok.numberValue;
            Advance();
            return true;
        }

        if (tok.type == TokenType::Identifier) {
            std::wstring func = tok.stringValue;
            Advance();
            if (!Match(TokenType::LParen)) return false;

            double arg1 = 0.0;
            if (!Expression(arg1)) return false;

            if (Match(TokenType::Comma)) {
                // Two-argument functions: pow(x, y), atan2(y, x), hypot(x, y)
                double arg2 = 0.0;
                if (!Expression(arg2)) return false;
                if (!Match(TokenType::RParen)) return false;

                if (func == L"pow") {
                    result = std::pow(arg1, arg2);
                } else if (func == L"atan2") {
                    result = std::atan2(arg1, arg2);
                } else if (func == L"hypot") {
                    result = std::hypot(arg1, arg2);
                } else {
                    return false;
                }
                return !std::isnan(result) && !std::isinf(result);
            }

            if (!Match(TokenType::RParen)) return false;

            if (func == L"sqrt") {
                if (arg1 < 0.0) return false;
                result = std::sqrt(arg1);
            } else if (func == L"cbrt") {
                result = std::cbrt(arg1);
            } else if (func == L"abs" || func == L"fabs") {
                result = std::abs(arg1);
            } else if (func == L"sin") {
                result = std::sin(arg1);
            } else if (func == L"cos") {
                result = std::cos(arg1);
            } else if (func == L"tan") {
                result = std::tan(arg1);
            } else if (func == L"asin") {
                if (arg1 < -1.0 || arg1 > 1.0) return false;
                result = std::asin(arg1);
            } else if (func == L"acos") {
                if (arg1 < -1.0 || arg1 > 1.0) return false;
                result = std::acos(arg1);
            } else if (func == L"atan") {
                result = std::atan(arg1);
            } else if (func == L"sinh") {
                result = std::sinh(arg1);
            } else if (func == L"cosh") {
                result = std::cosh(arg1);
            } else if (func == L"tanh") {
                result = std::tanh(arg1);
            } else if (func == L"ln") {
                if (arg1 <= 0.0) return false;
                result = std::log(arg1);
            } else if (func == L"log" || func == L"log10") {
                if (arg1 <= 0.0) return false;
                result = std::log10(arg1);
            } else if (func == L"log2") {
                if (arg1 <= 0.0) return false;
                result = std::log2(arg1);
            } else if (func == L"exp") {
                result = std::exp(arg1);
            } else if (func == L"ceil") {
                result = std::ceil(arg1);
            } else if (func == L"floor") {
                result = std::floor(arg1);
            } else if (func == L"round") {
                result = std::round(arg1);
            } else {
                return false;
            }
            return !std::isnan(result) && !std::isinf(result);
        }

        if (Match(TokenType::LParen)) {
            if (!Expression(result)) return false;
            return Match(TokenType::RParen);
        }

        return false;
    }

    std::vector<Token> tokens_;
    size_t cursor_;
};

} // namespace detail

inline std::optional<CalculationResult> EvaluateExpression(std::wstring_view input) {
    if (input.empty()) return std::nullopt;

    detail::Lexer lexer(input);
    std::vector<detail::Token> tokens;
    bool hasMathIndicator = false;
    bool hasDigitOrConst = false;

    if (!lexer.Tokenize(tokens, hasMathIndicator, hasDigitOrConst)) {
        return std::nullopt;
    }

    // Must have at least one operator/function/constant/= AND at least one digit or constant.
    // Bare numbers alone (like "42" or "7") without operators or '=' are treated as normal search queries.
    if (!hasMathIndicator || !hasDigitOrConst) {
        return std::nullopt;
    }

    detail::Parser parser(std::move(tokens));
    double value = 0.0;
    if (!parser.Parse(value)) {
        return std::nullopt;
    }

    if (std::isnan(value) || std::isinf(value)) {
        return std::nullopt;
    }

    CalculationResult result;
    result.value = value;
    result.expression = std::wstring(input);
    result.rawResult = detail::FormatNumber(value);
    result.formattedResult = detail::FormatNumber(value);

    if (result.rawResult.empty() || result.formattedResult.empty()) {
        return std::nullopt;
    }

    return result;
}

} // namespace takeoff

