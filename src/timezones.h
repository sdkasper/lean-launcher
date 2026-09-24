#pragma once

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Time zone conversion (US-048): "time in Tokyo", "10am PST in CET",
// "15:00 London in New York", "3pm in Berlin". Uses Windows' own zone rules
// (kept current by Windows Update, correct per year for DST) plus a built-in
// name table - no ICU, no embedded tz database. Times need am/pm or a colon,
// so "5 in berlin" or "10 in in cm" never read as times.
namespace leanlauncher {
namespace timezones {

struct Place {
    const wchar_t* name;     // lowercase alias as typed
    const wchar_t* zoneKey;  // Windows time zone key
    const wchar_t* display;  // shown in results
    const wchar_t* note;     // the stated reading of an abbreviation, or nullptr for cities
};

inline constexpr Place kPlaces[] = {
    // UTC
    {L"utc", L"UTC", L"UTC", nullptr}, {L"gmt", L"UTC", L"UTC", L"GMT treated as UTC"}, {L"z", L"UTC", L"UTC", nullptr},
    // Europe
    {L"london", L"GMT Standard Time", L"London", nullptr}, {L"uk", L"GMT Standard Time", L"London", nullptr},
    {L"dublin", L"GMT Standard Time", L"Dublin", nullptr}, {L"lisbon", L"GMT Standard Time", L"Lisbon", nullptr},
    {L"bst", L"GMT Standard Time", L"London", L"BST treated as British Summer Time (London)"},
    {L"wet", L"GMT Standard Time", L"Lisbon", L"WET treated as Western European Time (Lisbon)"},
    {L"cet", L"W. Europe Standard Time", L"Berlin", L"CET treated as Central European Time (Berlin)"},
    {L"cest", L"W. Europe Standard Time", L"Berlin", L"CEST treated as Central European Time (Berlin)"},
    {L"berlin", L"W. Europe Standard Time", L"Berlin", nullptr}, {L"munich", L"W. Europe Standard Time", L"Munich", nullptr},
    {L"frankfurt", L"W. Europe Standard Time", L"Frankfurt", nullptr}, {L"hamburg", L"W. Europe Standard Time", L"Hamburg", nullptr},
    {L"germany", L"W. Europe Standard Time", L"Berlin", nullptr}, {L"amsterdam", L"W. Europe Standard Time", L"Amsterdam", nullptr},
    {L"rome", L"W. Europe Standard Time", L"Rome", nullptr}, {L"milan", L"W. Europe Standard Time", L"Milan", nullptr},
    {L"vienna", L"W. Europe Standard Time", L"Vienna", nullptr}, {L"zurich", L"W. Europe Standard Time", L"Zurich", nullptr},
    {L"geneva", L"W. Europe Standard Time", L"Geneva", nullptr}, {L"stockholm", L"W. Europe Standard Time", L"Stockholm", nullptr},
    {L"oslo", L"W. Europe Standard Time", L"Oslo", nullptr}, {L"paris", L"Romance Standard Time", L"Paris", nullptr},
    {L"brussels", L"Romance Standard Time", L"Brussels", nullptr}, {L"madrid", L"Romance Standard Time", L"Madrid", nullptr},
    {L"barcelona", L"Romance Standard Time", L"Barcelona", nullptr}, {L"copenhagen", L"Romance Standard Time", L"Copenhagen", nullptr},
    {L"prague", L"Central Europe Standard Time", L"Prague", nullptr}, {L"budapest", L"Central Europe Standard Time", L"Budapest", nullptr},
    {L"belgrade", L"Central Europe Standard Time", L"Belgrade", nullptr}, {L"warsaw", L"Central European Standard Time", L"Warsaw", nullptr},
    {L"zagreb", L"Central European Standard Time", L"Zagreb", nullptr},
    {L"eet", L"GTB Standard Time", L"Athens", L"EET treated as Eastern European Time (Athens)"},
    {L"athens", L"GTB Standard Time", L"Athens", nullptr}, {L"bucharest", L"GTB Standard Time", L"Bucharest", nullptr},
    {L"helsinki", L"FLE Standard Time", L"Helsinki", nullptr}, {L"kyiv", L"FLE Standard Time", L"Kyiv", nullptr},
    {L"kiev", L"FLE Standard Time", L"Kyiv", nullptr}, {L"riga", L"FLE Standard Time", L"Riga", nullptr},
    {L"tallinn", L"FLE Standard Time", L"Tallinn", nullptr}, {L"vilnius", L"FLE Standard Time", L"Vilnius", nullptr},
    {L"sofia", L"FLE Standard Time", L"Sofia", nullptr}, {L"istanbul", L"Turkey Standard Time", L"Istanbul", nullptr},
    {L"moscow", L"Russian Standard Time", L"Moscow", nullptr}, {L"msk", L"Russian Standard Time", L"Moscow", L"MSK treated as Moscow Time"},
    // North America
    {L"est", L"Eastern Standard Time", L"New York", L"EST treated as US Eastern Time"},
    {L"edt", L"Eastern Standard Time", L"New York", L"EDT treated as US Eastern Time"},
    {L"et", L"Eastern Standard Time", L"New York", L"ET treated as US Eastern Time"},
    {L"eastern", L"Eastern Standard Time", L"New York", L"Eastern treated as US Eastern Time"},
    {L"new york", L"Eastern Standard Time", L"New York", nullptr}, {L"nyc", L"Eastern Standard Time", L"New York", nullptr},
    {L"boston", L"Eastern Standard Time", L"Boston", nullptr}, {L"washington", L"Eastern Standard Time", L"Washington", nullptr},
    {L"miami", L"Eastern Standard Time", L"Miami", nullptr}, {L"atlanta", L"Eastern Standard Time", L"Atlanta", nullptr},
    {L"toronto", L"Eastern Standard Time", L"Toronto", nullptr}, {L"montreal", L"Eastern Standard Time", L"Montreal", nullptr},
    {L"detroit", L"Eastern Standard Time", L"Detroit", nullptr},
    {L"cst", L"Central Standard Time", L"Chicago", L"CST treated as US Central Time"},
    {L"cdt", L"Central Standard Time", L"Chicago", L"CDT treated as US Central Time"},
    {L"ct", L"Central Standard Time", L"Chicago", L"CT treated as US Central Time"},
    {L"central", L"Central Standard Time", L"Chicago", L"Central treated as US Central Time"},
    {L"chicago", L"Central Standard Time", L"Chicago", nullptr}, {L"dallas", L"Central Standard Time", L"Dallas", nullptr},
    {L"houston", L"Central Standard Time", L"Houston", nullptr}, {L"austin", L"Central Standard Time", L"Austin", nullptr},
    {L"mexico city", L"Central Standard Time (Mexico)", L"Mexico City", nullptr},
    {L"mst", L"Mountain Standard Time", L"Denver", L"MST treated as US Mountain Time"},
    {L"mdt", L"Mountain Standard Time", L"Denver", L"MDT treated as US Mountain Time"},
    {L"mt", L"Mountain Standard Time", L"Denver", L"MT treated as US Mountain Time"},
    {L"denver", L"Mountain Standard Time", L"Denver", nullptr}, {L"calgary", L"Mountain Standard Time", L"Calgary", nullptr},
    {L"phoenix", L"US Mountain Standard Time", L"Phoenix", nullptr},
    {L"pst", L"Pacific Standard Time", L"Los Angeles", L"PST treated as Pacific Time"},
    {L"pdt", L"Pacific Standard Time", L"Los Angeles", L"PDT treated as Pacific Time"},
    {L"pt", L"Pacific Standard Time", L"Los Angeles", L"PT treated as Pacific Time"},
    {L"pacific", L"Pacific Standard Time", L"Los Angeles", L"Pacific treated as US Pacific Time"},
    {L"los angeles", L"Pacific Standard Time", L"Los Angeles", nullptr}, {L"la", L"Pacific Standard Time", L"Los Angeles", nullptr},
    {L"san francisco", L"Pacific Standard Time", L"San Francisco", nullptr}, {L"sf", L"Pacific Standard Time", L"San Francisco", nullptr},
    {L"seattle", L"Pacific Standard Time", L"Seattle", nullptr}, {L"vancouver", L"Pacific Standard Time", L"Vancouver", nullptr},
    {L"portland", L"Pacific Standard Time", L"Portland", nullptr}, {L"las vegas", L"Pacific Standard Time", L"Las Vegas", nullptr},
    {L"akst", L"Alaskan Standard Time", L"Anchorage", L"AKST treated as Alaska Time"},
    {L"anchorage", L"Alaskan Standard Time", L"Anchorage", nullptr},
    {L"hst", L"Hawaiian Standard Time", L"Honolulu", L"HST treated as Hawaii Time"},
    {L"honolulu", L"Hawaiian Standard Time", L"Honolulu", nullptr}, {L"hawaii", L"Hawaiian Standard Time", L"Honolulu", nullptr},
    // South America
    {L"sao paulo", L"E. South America Standard Time", L"São Paulo", nullptr},
    {L"são paulo", L"E. South America Standard Time", L"São Paulo", nullptr},
    {L"rio", L"E. South America Standard Time", L"Rio de Janeiro", nullptr},
    {L"rio de janeiro", L"E. South America Standard Time", L"Rio de Janeiro", nullptr},
    {L"buenos aires", L"Argentina Standard Time", L"Buenos Aires", nullptr},
    {L"santiago", L"Pacific SA Standard Time", L"Santiago", nullptr}, {L"bogota", L"SA Pacific Standard Time", L"Bogotá", nullptr},
    {L"lima", L"SA Pacific Standard Time", L"Lima", nullptr},
    // Africa and the Middle East
    {L"cairo", L"Egypt Standard Time", L"Cairo", nullptr}, {L"johannesburg", L"South Africa Standard Time", L"Johannesburg", nullptr},
    {L"cape town", L"South Africa Standard Time", L"Cape Town", nullptr},
    {L"sast", L"South Africa Standard Time", L"Johannesburg", L"SAST treated as South Africa Time"},
    {L"lagos", L"W. Central Africa Standard Time", L"Lagos", nullptr}, {L"nairobi", L"E. Africa Standard Time", L"Nairobi", nullptr},
    {L"jerusalem", L"Israel Standard Time", L"Jerusalem", nullptr}, {L"tel aviv", L"Israel Standard Time", L"Tel Aviv", nullptr},
    {L"dubai", L"Arabian Standard Time", L"Dubai", nullptr}, {L"abu dhabi", L"Arabian Standard Time", L"Abu Dhabi", nullptr},
    {L"riyadh", L"Arab Standard Time", L"Riyadh", nullptr}, {L"doha", L"Arab Standard Time", L"Doha", nullptr},
    {L"tehran", L"Iran Standard Time", L"Tehran", nullptr},
    // Asia
    {L"karachi", L"Pakistan Standard Time", L"Karachi", nullptr},
    {L"ist", L"India Standard Time", L"New Delhi", L"IST treated as India Standard Time"},
    {L"india", L"India Standard Time", L"New Delhi", nullptr}, {L"delhi", L"India Standard Time", L"New Delhi", nullptr},
    {L"new delhi", L"India Standard Time", L"New Delhi", nullptr}, {L"mumbai", L"India Standard Time", L"Mumbai", nullptr},
    {L"bangalore", L"India Standard Time", L"Bengaluru", nullptr}, {L"bengaluru", L"India Standard Time", L"Bengaluru", nullptr},
    {L"kolkata", L"India Standard Time", L"Kolkata", nullptr}, {L"chennai", L"India Standard Time", L"Chennai", nullptr},
    {L"hyderabad", L"India Standard Time", L"Hyderabad", nullptr}, {L"dhaka", L"Bangladesh Standard Time", L"Dhaka", nullptr},
    {L"bangkok", L"SE Asia Standard Time", L"Bangkok", nullptr}, {L"jakarta", L"SE Asia Standard Time", L"Jakarta", nullptr},
    {L"hanoi", L"SE Asia Standard Time", L"Hanoi", nullptr}, {L"ho chi minh city", L"SE Asia Standard Time", L"Ho Chi Minh City", nullptr},
    {L"singapore", L"Singapore Standard Time", L"Singapore", nullptr}, {L"sgt", L"Singapore Standard Time", L"Singapore", L"SGT treated as Singapore Time"},
    {L"kuala lumpur", L"Singapore Standard Time", L"Kuala Lumpur", nullptr}, {L"manila", L"Singapore Standard Time", L"Manila", nullptr},
    {L"hong kong", L"China Standard Time", L"Hong Kong", nullptr}, {L"hkt", L"China Standard Time", L"Hong Kong", L"HKT treated as Hong Kong Time"},
    {L"beijing", L"China Standard Time", L"Beijing", nullptr}, {L"shanghai", L"China Standard Time", L"Shanghai", nullptr},
    {L"shenzhen", L"China Standard Time", L"Shenzhen", nullptr}, {L"china", L"China Standard Time", L"Beijing", nullptr},
    {L"taipei", L"Taipei Standard Time", L"Taipei", nullptr},
    {L"tokyo", L"Tokyo Standard Time", L"Tokyo", nullptr}, {L"osaka", L"Tokyo Standard Time", L"Osaka", nullptr},
    {L"japan", L"Tokyo Standard Time", L"Tokyo", nullptr}, {L"jst", L"Tokyo Standard Time", L"Tokyo", L"JST treated as Japan Standard Time"},
    {L"seoul", L"Korea Standard Time", L"Seoul", nullptr}, {L"korea", L"Korea Standard Time", L"Seoul", nullptr},
    {L"kst", L"Korea Standard Time", L"Seoul", L"KST treated as Korea Standard Time"},
    // Oceania
    {L"sydney", L"AUS Eastern Standard Time", L"Sydney", nullptr}, {L"melbourne", L"AUS Eastern Standard Time", L"Melbourne", nullptr},
    {L"canberra", L"AUS Eastern Standard Time", L"Canberra", nullptr},
    {L"aest", L"AUS Eastern Standard Time", L"Sydney", L"AEST treated as Australian Eastern Time (Sydney)"},
    {L"aedt", L"AUS Eastern Standard Time", L"Sydney", L"AEDT treated as Australian Eastern Time (Sydney)"},
    {L"brisbane", L"E. Australia Standard Time", L"Brisbane", nullptr}, {L"adelaide", L"Cen. Australia Standard Time", L"Adelaide", nullptr},
    {L"perth", L"W. Australia Standard Time", L"Perth", nullptr},
    {L"awst", L"W. Australia Standard Time", L"Perth", L"AWST treated as Australian Western Time (Perth)"},
    {L"auckland", L"New Zealand Standard Time", L"Auckland", nullptr}, {L"wellington", L"New Zealand Standard Time", L"Wellington", nullptr},
    {L"nzst", L"New Zealand Standard Time", L"Auckland", L"NZST treated as New Zealand Time"},
    {L"nzdt", L"New Zealand Standard Time", L"Auckland", L"NZDT treated as New Zealand Time"},
};

namespace detail {

inline std::wstring Normalize(std::wstring_view s) {
    std::wstring out;
    bool lastSpace = true;
    for (wchar_t ch : s) {
        if (iswspace(ch)) {
            if (!lastSpace) out += L' ';
            lastSpace = true;
        } else {
            out += static_cast<wchar_t>(towlower(ch));
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// "10am", "10 am", "10:30pm", "15:00" at the start of `s`. Returns the length
// consumed, or 0. Requires am/pm or a colon.
inline size_t ParseTime(std::wstring_view s, int& hour, int& minute) {
    size_t i = 0;
    int h = 0;
    size_t digits = 0;
    while (i < s.size() && iswdigit(s[i]) && digits < 2) { h = h * 10 + (s[i] - L'0'); ++i; ++digits; }
    if (digits == 0 || (i < s.size() && iswdigit(s[i]))) return 0;
    int m = 0;
    bool colon = false;
    if (i < s.size() && s[i] == L':') {
        if (i + 2 >= s.size() || !iswdigit(s[i + 1]) || !iswdigit(s[i + 2])) return 0;
        m = (s[i + 1] - L'0') * 10 + (s[i + 2] - L'0');
        i += 3;
        colon = true;
    }
    size_t j = i;
    if (j < s.size() && s[j] == L' ') ++j;
    bool am = false, pm = false;
    if (s.substr(j, 2) == L"am") { am = true; j += 2; }
    else if (s.substr(j, 2) == L"pm") { pm = true; j += 2; }
    if (am || pm) {
        i = j;
        if (h < 1 || h > 12) return 0;
        if (am) h = (h == 12) ? 0 : h;
        if (pm) h = (h == 12) ? 12 : h + 12;
    } else if (!colon) {
        return 0;
    }
    if (h > 23 || m > 59) return 0;
    if (i < s.size() && s[i] != L' ') return 0;
    hour = h;
    minute = m;
    return i;
}

inline bool ToFileTime(const SYSTEMTIME& st, ULONGLONG& out) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    out = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return true;
}

inline SYSTEMTIME FromFileTime(ULONGLONG value) {
    FILETIME ft{static_cast<DWORD>(value & 0xFFFFFFFF), static_cast<DWORD>(value >> 32)};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    return st;
}

inline bool SameMinute(const SYSTEMTIME& a, const SYSTEMTIME& b) {
    return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay && a.wHour == b.wHour && a.wMinute == b.wMinute;
}

inline constexpr ULONGLONG kTicksPerMinute = 600000000ULL;

inline std::wstring Clock(const SYSTEMTIME& t) {
    wchar_t buffer[16];
    swprintf(buffer, std::size(buffer), L"%02u:%02u", t.wHour, t.wMinute);
    return buffer;
}

inline std::wstring DateLabel(const SYSTEMTIME& t) {
    static constexpr const wchar_t* kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    static constexpr const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                                 L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    ULONGLONG ticks = 0;
    ToFileTime(t, ticks);
    const SYSTEMTIME withWeekday = FromFileTime(ticks);
    wchar_t buffer[32];
    swprintf(buffer, std::size(buffer), L"%s %u %s", kDays[withWeekday.wDayOfWeek % 7], t.wDay,
             kMonths[(t.wMonth + 11) % 12]);
    return buffer;
}

inline std::wstring OffsetLabel(int minutes) {
    wchar_t buffer[16];
    const int magnitude = minutes < 0 ? -minutes : minutes;
    if (magnitude % 60 == 0) swprintf(buffer, std::size(buffer), L"UTC%c%d", minutes < 0 ? L'-' : L'+', magnitude / 60);
    else swprintf(buffer, std::size(buffer), L"UTC%c%d:%02d", minutes < 0 ? L'-' : L'+', magnitude / 60, magnitude % 60);
    return buffer;
}

} // namespace detail

inline const Place* FindPlace(std::wstring_view lowered) {
    for (const auto& place : kPlaces) {
        if (lowered == place.name) return &place;
    }
    return nullptr;
}

struct TimeQuery {
    enum class Kind { Now, Convert } kind = Kind::Now;
    int hour = 0;
    int minute = 0;
    const Place* from = nullptr;  // nullptr: the user's local time zone
    const Place* to = nullptr;
    std::wstring timeText;        // as typed, e.g. "10am"
};

inline std::optional<TimeQuery> ParseTimeQuery(std::wstring_view query) {
    const std::wstring text = detail::Normalize(query);
    TimeQuery q;
    if (text.rfind(L"time in ", 0) == 0) {
        q.to = FindPlace(text.substr(8));
        if (!q.to) return std::nullopt;
        q.kind = TimeQuery::Kind::Now;
        return q;
    }
    const size_t in = text.rfind(L" in ");
    const size_t to = text.rfind(L" to ");
    size_t sep = std::wstring::npos;
    if (in != std::wstring::npos && (to == std::wstring::npos || in > to)) sep = in;
    else sep = to;
    if (sep == std::wstring::npos) return std::nullopt;
    q.to = FindPlace(text.substr(sep + 4));
    if (!q.to) return std::nullopt;
    const std::wstring left = text.substr(0, sep);
    const size_t used = detail::ParseTime(left, q.hour, q.minute);
    if (used == 0) return std::nullopt;
    q.timeText = left.substr(0, used);
    std::wstring rest = left.substr(used);
    while (!rest.empty() && rest.front() == L' ') rest.erase(rest.begin());
    if (!rest.empty()) {
        q.from = FindPlace(rest);
        if (!q.from) return std::nullopt;
    }
    q.kind = TimeQuery::Kind::Convert;
    return q;
}

// Windows' zone list, read once on first use and freed with Clear() (NFR-018).
class ZoneCache {
public:
    bool Load() {
        if (!zones_.empty()) return true;
        for (DWORD i = 0;; ++i) {
            DYNAMIC_TIME_ZONE_INFORMATION zone{};
            if (EnumDynamicTimeZoneInformation(i, &zone) != ERROR_SUCCESS) break;
            zones_.push_back(zone);
        }
        return !zones_.empty();
    }
    const DYNAMIC_TIME_ZONE_INFORMATION* Find(std::wstring_view key) const {
        for (const auto& zone : zones_) {
            if (_wcsnicmp(zone.TimeZoneKeyName, key.data(), key.size()) == 0 && zone.TimeZoneKeyName[key.size()] == 0) {
                return &zone;
            }
        }
        return nullptr;
    }
    void Clear() { std::vector<DYNAMIC_TIME_ZONE_INFORMATION>().swap(zones_); }
    bool Loaded() const { return !zones_.empty(); }

private:
    std::vector<DYNAMIC_TIME_ZONE_INFORMATION> zones_;
};

enum class LocalStatus { Ok, Nonexistent, Ambiguous };

inline bool UtcToZoneLocal(const DYNAMIC_TIME_ZONE_INFORMATION& zone, const SYSTEMTIME& utc, SYSTEMTIME& local) {
    return SystemTimeToTzSpecificLocalTimeEx(&zone, &utc, &local) != FALSE;
}

// Wall-clock time in `zone` to UTC, flagging times that fall in a DST gap
// (clocks go forward) or happen twice (clocks go back).
inline bool ZoneLocalToUtc(const DYNAMIC_TIME_ZONE_INFORMATION& zone, const SYSTEMTIME& local, SYSTEMTIME& utc,
                           LocalStatus& status) {
    if (!TzSpecificLocalTimeToSystemTimeEx(&zone, &local, &utc)) return false;
    status = LocalStatus::Ok;
    SYSTEMTIME back{};
    if (!UtcToZoneLocal(zone, utc, back)) return false;
    if (!detail::SameMinute(back, local)) {
        status = LocalStatus::Nonexistent;
        return true;
    }
    ULONGLONG ticks = 0;
    if (!detail::ToFileTime(utc, ticks)) return false;
    for (long long shift : {-60LL, 60LL}) {
        const SYSTEMTIME other = detail::FromFileTime(ticks + static_cast<ULONGLONG>(shift) * detail::kTicksPerMinute);
        SYSTEMTIME otherLocal{};
        if (UtcToZoneLocal(zone, other, otherLocal) && detail::SameMinute(otherLocal, local)) {
            status = LocalStatus::Ambiguous;
            break;
        }
    }
    return true;
}

inline int OffsetMinutes(const DYNAMIC_TIME_ZONE_INFORMATION& zone, const SYSTEMTIME& utc) {
    SYSTEMTIME local{};
    ULONGLONG a = 0, b = 0;
    if (!UtcToZoneLocal(zone, utc, local) || !detail::ToFileTime(local, a) || !detail::ToFileTime(utc, b)) return 0;
    return static_cast<int>((static_cast<long long>(a) - static_cast<long long>(b)) / static_cast<long long>(detail::kTicksPerMinute));
}

struct TimeRow {
    std::wstring text;       // "02:00 Tokyo (Thu 2 Jul)"
    std::wstring valueText;  // "02:00" - what Enter copies
    std::wstring source;     // "10am PST"
    std::wstring note;       // "PST treated as Pacific Time (UTC-7 on this date)"
};

inline std::wstring DescribePlace(const Place* place, int offsetMinutes) {
    if (!place) return L"Your local time (" + detail::OffsetLabel(offsetMinutes) + L" on this date)";
    if (place->note) return std::wstring(place->note) + L" (" + detail::OffsetLabel(offsetMinutes) + L" on this date)";
    return std::wstring(place->display) + L", " + detail::OffsetLabel(offsetMinutes);
}

inline std::optional<TimeRow> EvaluateTimeQuery(std::wstring_view query, const SYSTEMTIME& nowUtc, ZoneCache& zones) {
    const auto q = ParseTimeQuery(query);
    if (!q || !zones.Load()) return std::nullopt;
    const DYNAMIC_TIME_ZONE_INFORMATION* target = zones.Find(q->to->zoneKey);
    if (!target) return std::nullopt;
    TimeRow row;
    if (q->kind == TimeQuery::Kind::Now) {
        SYSTEMTIME local{};
        if (!UtcToZoneLocal(*target, nowUtc, local)) return std::nullopt;
        row.valueText = detail::Clock(local);
        row.text = row.valueText + L" " + q->to->display + L" (" + detail::DateLabel(local) + L")";
        row.source = L"time in " + std::wstring(q->to->display);
        row.note = DescribePlace(q->to, OffsetMinutes(*target, nowUtc));
        return row;
    }
    DYNAMIC_TIME_ZONE_INFORMATION localZone{};
    const DYNAMIC_TIME_ZONE_INFORMATION* source = nullptr;
    if (q->from) {
        source = zones.Find(q->from->zoneKey);
    } else if (GetDynamicTimeZoneInformation(&localZone) != TIME_ZONE_ID_INVALID) {
        source = &localZone;
    }
    if (!source) return std::nullopt;
    SYSTEMTIME sourceToday{};
    if (!UtcToZoneLocal(*source, nowUtc, sourceToday)) return std::nullopt;
    SYSTEMTIME sourceLocal = sourceToday;
    sourceLocal.wHour = static_cast<WORD>(q->hour);
    sourceLocal.wMinute = static_cast<WORD>(q->minute);
    sourceLocal.wSecond = sourceLocal.wMilliseconds = 0;
    SYSTEMTIME utc{}, targetLocal{};
    LocalStatus status = LocalStatus::Ok;
    if (!ZoneLocalToUtc(*source, sourceLocal, utc, status) || !UtcToZoneLocal(*target, utc, targetLocal)) return std::nullopt;
    row.valueText = detail::Clock(targetLocal);
    row.text = row.valueText + L" " + q->to->display;
    if (targetLocal.wDay != sourceLocal.wDay || targetLocal.wMonth != sourceLocal.wMonth) {
        row.text += L" (" + detail::DateLabel(targetLocal) + L")";
    }
    const std::wstring sourceName = q->from ? std::wstring(q->from->display) : std::wstring(L"your local time");
    row.source = detail::Clock(sourceLocal) + L" " + (q->from ? std::wstring(q->from->name) : std::wstring(L"local"));
    if (status == LocalStatus::Nonexistent) {
        row.note = detail::Clock(sourceLocal) + L" doesn't exist in " + sourceName + L" on this date (clocks go forward)";
    } else if (status == LocalStatus::Ambiguous) {
        row.note = detail::Clock(sourceLocal) + L" happens twice in " + sourceName + L" on this date (clocks go back); showing the first";
    } else {
        row.note = DescribePlace(q->from, OffsetMinutes(*source, utc));
    }
    return row;
}

} // namespace timezones
} // namespace leanlauncher
