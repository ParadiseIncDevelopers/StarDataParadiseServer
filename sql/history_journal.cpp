#include "history_journal.hpp"

#include <sdp/wire.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace sdp::sql
{
HistoryUnavailable::HistoryUnavailable(uint64_t floor)
    : Error(Code::range, {}, "History revision is outside the recoverable window"), floor_(floor)
{
}

HistoryPinned::HistoryPinned(HistoryPin pin)
    : Error(Code::constraint, {}, "History retention is restricted by a pin"), pin_(std::move(pin))
{
}

namespace
{
using wire::Json;
using Digest = std::array<disk::Byte, 32>;
constexpr uint32_t outcome_schema = 0x53444c31;
constexpr uint32_t security_schema = 0x53444131;
constexpr uint64_t max_quota = disk::wal_quota;
constexpr size_t max_pins = 64;
constexpr int64_t history_format = 1;
constexpr int64_t legacy_outcome_ttl_seconds = 24 * 60 * 60;

[[noreturn]] void corrupt(const char* reason)
{
    throw disk::Error(disk::Fault::corrupt, reason);
}

class Sha
{
public:
    Sha() : ctx_(EVP_MD_CTX_new(), EVP_MD_CTX_free)
    {
        if (!ctx_ || EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1)
            throw disk::Error(disk::Fault::io, "Unable to initialize History SHA-256");
    }

    void add(std::span<const disk::Byte> bytes)
    {
        if (!bytes.empty() && EVP_DigestUpdate(ctx_.get(), bytes.data(), bytes.size()) != 1)
            throw disk::Error(disk::Fault::io, "Unable to update History SHA-256");
    }

    void text(std::string_view value)
    {
        add({reinterpret_cast<const disk::Byte*>(value.data()), value.size()});
    }

    template<typename Number>
    void number(Number value)
    {
        std::array<disk::Byte, sizeof(Number)> bytes{};
        disk::put<Number>(bytes, 0, value);
        add(bytes);
    }

    Digest finish()
    {
        Digest digest{};
        unsigned bytes{};
        if (EVP_DigestFinal_ex(ctx_.get(), digest.data(), &bytes) != 1 || bytes != digest.size())
            throw disk::Error(disk::Fault::io, "Unable to finalize History SHA-256");
        return digest;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_;
};

std::string hex(std::span<const disk::Byte> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

uint32_t schema(std::span<const disk::Byte> bytes)
{
    if (bytes.empty()) return 0;
    return disk::load_le<uint32_t>(bytes, 4);
}

bool excluded_from_quota(uint32_t kind)
{
    return kind == history_seal_schema || kind == history_policy_schema || kind == outcome_schema ||
        kind == history_receipt_schema;
}

uint32_t schema(const disk::Change& change)
{
    const auto before = schema(change.before);
    const auto after = schema(change.after);
    if (before && after && before != after &&
        (excluded_from_quota(before) || excluded_from_quota(after) || before == security_schema || after == security_schema))
        corrupt("A reserved History record changed its schema");
    return after ? after : before;
}

uint64_t journal_bytes(std::span<const disk::Change> changes)
{
    uint64_t bytes{};
    for (const auto& item : changes)
        if (!excluded_from_quota(schema(item))) bytes += 20 + item.before.size() + item.after.size();
    return bytes;
}

bool outcome_only(std::span<const disk::Change> changes)
{
    return changes.empty() || std::ranges::all_of(changes, [](const disk::Change& item) {
        return item.before.empty() && schema(item.after) == outcome_schema;
    });
}

bool short_name(std::string_view name)
{
    return !name.empty() && name.size() <= 64 &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == name.npos;
}

bool pin_kind(std::string_view kind)
{
    return kind == "snapshot" || kind == "dataset" || kind == "backup" || kind == "restore";
}

template<typename T>
const T& field(const disk::Row& row, size_t index)
{
    if (index >= row.values.size()) corrupt("Invalid History record width");
    const auto* value = std::get_if<T>(&row.values[index]);
    if (!value) corrupt("Invalid History record type");
    return *value;
}

Digest field_digest(const disk::Row& row, size_t index)
{
    const auto& data = field<disk::Bytes>(row, index);
    Digest digest{};
    if (data.size() != digest.size()) corrupt("Invalid History digest length");
    std::ranges::copy(data, digest.begin());
    return digest;
}

struct Seal
{
    uint64_t lsn;
    uint64_t parent;
    std::string actor;
    int64_t utc;
    Digest prior;
    Digest hash;
};

Seal read_seal(const disk::Change& change)
{
    if (change.kind != disk::ChangeKind::catalog || !change.before.empty() || change.after.empty())
        corrupt("History seal was modified");
    const auto row = disk::decode(change.after);
    if (row.schema != history_seal_schema || row.values.size() != 7 || field<int64_t>(row, 0) != history_format)
        corrupt("Invalid History seal format");
    const auto lsn = field<int64_t>(row, 1);
    const auto parent = field<int64_t>(row, 2);
    const auto& actor = field<std::string>(row, 3);
    const auto utc = field<int64_t>(row, 4);
    if (lsn <= 0 || parent < 0 || parent != lsn - 1 || !short_name(actor) || utc <= 0)
        corrupt("Invalid History seal metadata");
    return {static_cast<uint64_t>(lsn), static_cast<uint64_t>(parent), actor, utc,
        field_digest(row, 5), field_digest(row, 6)};
}

Digest hash_changes(const Seal& seal, std::span<const disk::Change> changes, bool contains_seal)
{
    Sha sha;
    sha.text("StarDataParadise.History.v1");
    sha.number(seal.lsn);
    sha.number(seal.parent);
    sha.number(static_cast<uint64_t>(seal.utc));
    sha.number(static_cast<uint32_t>(seal.actor.size()));
    sha.text(seal.actor);
    sha.add(seal.prior);
    sha.number(static_cast<uint32_t>(changes.size() - static_cast<size_t>(contains_seal)));
    for (const auto& change : changes)
    {
        if (schema(change) == history_seal_schema) continue;
        sha.number(change.id);
        sha.number(static_cast<uint32_t>(change.kind));
        sha.number(static_cast<uint32_t>(change.before.size()));
        sha.number(static_cast<uint32_t>(change.after.size()));
        sha.add(change.before);
        sha.add(change.after);
    }
    return sha.finish();
}

struct LegacyIdentity
{
    std::string actor;
    std::optional<int64_t> utc;
};

LegacyIdentity legacy_identity(const disk::Change& change, uint64_t revision)
{
    if (!change.before.empty() || change.after.empty()) corrupt("Invalid commit outcome record in History");
    const auto row = disk::decode(change.after);
    if (row.values.size() != 4) corrupt("Invalid commit outcome width in History");
    const auto& actor = field<std::string>(row, 0);
    const auto& token = field<std::string>(row, 1);
    const auto lsn = field<int64_t>(row, 2);
    const auto expires = field<int64_t>(row, 3);
    if (!short_name(actor) || token.size() != 32 || token.find_first_not_of("0123456789abcdef") != token.npos ||
        lsn <= 0 || static_cast<uint64_t>(lsn) != revision || expires <= 0)
        corrupt("Invalid commit outcome metadata in History");
    return {actor, expires > legacy_outcome_ttl_seconds ?
        std::optional(expires - legacy_outcome_ttl_seconds) : std::nullopt};
}

Digest scan_history(disk::Engine& engine, HistoryState& state, uint64_t first, uint64_t last,
    const HistoryJournal::Visitor& visitor, const WorkCheck& check)
{
    Digest previous{};
    bool saw_seal{};
    engine.visit_history([&](disk::Revision&& frame) {
        if (frame.lsn >= state.floor) state.retained_bytes += journal_bytes(frame.changes);
        std::optional<Seal> seal;
        std::optional<LegacyIdentity> legacy;
        for (const auto& change : frame.changes)
        {
            const auto kind = schema(change);
            if (kind == history_seal_schema)
            {
                if (seal) corrupt("A revision contains multiple History seals");
                seal = read_seal(change);
            }
            if (kind == outcome_schema)
            {
                if (legacy) corrupt("A revision contains multiple commit outcome records");
                legacy = legacy_identity(change, frame.lsn);
            }
        }
        if (seal)
        {
            if (seal->lsn != frame.lsn || seal->prior != previous || (legacy && legacy->actor != seal->actor) ||
                seal->hash != hash_changes(*seal, frame.changes, true))
                corrupt("History SHA-256 seal mismatch");
            previous = seal->hash;
            saw_seal = true;
        }
        else if (saw_seal && !outcome_only(frame.changes))
            corrupt("A committed business revision is missing its History seal");

        if (visitor && frame.lsn >= first && frame.lsn <= last)
        {
            // Keep authentication verifiers and metadata tokens out of every reader.
            std::erase_if(frame.changes, [](const auto& item) {
                const auto kind = schema(item);
                return excluded_from_quota(kind) || kind == security_schema;
            });
            visitor({frame.lsn, frame.lsn - 1, frame.lsn, seal ? seal->actor : legacy ? std::move(legacy->actor) : std::string{},
                seal ? std::optional(seal->utc) : legacy ? legacy->utc : std::nullopt,
                seal ? hex(seal->hash) : std::string{},
                std::move(frame.changes)});
        }
    }, check);
    return previous;
}

std::vector<HistoryPin> decode_pins(const std::string& text)
{
    std::vector<HistoryPin> pins;
    try
    {
        const auto array = Json::parse(text);
        if (!array.is_array() || array.size() > max_pins) corrupt("Invalid History pins");
        std::set<std::pair<std::string, std::string>> unique;
        for (const auto& item : array)
        {
            auto name = item.at("name").get<std::string>();
            auto kind = item.at("kind").get<std::string>();
            if (!short_name(name) || !pin_kind(kind) || !unique.emplace(kind, name).second ||
                !item.at("revision").is_number_unsigned())
                corrupt("Invalid History pin");
            pins.push_back({std::move(name), std::move(kind), item.at("revision").get<uint64_t>()});
        }
    }
    catch (const Json::exception&) { corrupt("Invalid History pin encoding"); }
    return pins;
}
}

struct HistoryJournal::Settings
{
    HistoryState state;
    disk::RowId row{};
};

std::string history_sha256(std::span<const disk::Byte> bytes)
{
    Sha sha;
    sha.add(bytes);
    return hex(sha.finish());
}

HistoryJournal::HistoryJournal(disk::Engine& engine, disk::Transaction& tx, disk::Owner owner, WorkCheck check)
    : engine_(engine), tx_(tx), owner_(owner), check_(std::move(check))
{
}

HistoryJournal::Settings HistoryJournal::settings()
{
    Settings settings;
    settings.state.head = engine_.current().lsn;
    tx_.scan(owner_, 0, [&](disk::RowId id, const disk::Row& row) {
        if (settings.row || row.values.size() != 4 || field<int64_t>(row, 0) != history_format)
            corrupt("Invalid History retention record");
        settings.row = id;
        const auto floor = field<int64_t>(row, 1);
        const auto quota = field<int64_t>(row, 2);
        if (floor < 1 || static_cast<uint64_t>(floor) > settings.state.head + 1 || quota < 1 ||
            static_cast<uint64_t>(quota) > max_quota)
            corrupt("Invalid History floor or quota");
        settings.state.floor = static_cast<uint64_t>(floor);
        settings.state.quota = static_cast<uint64_t>(quota);
        settings.state.pins = decode_pins(field<std::string>(row, 3));
        for (const auto& pin : settings.state.pins)
            if (pin.revision < settings.state.floor || pin.revision > settings.state.head)
                corrupt("History pin lies outside the retained window");
        return true;
    }, history_policy_schema, check_);
    return settings;
}

void HistoryJournal::update_settings(const Settings& settings)
{
    Json pins = Json::array();
    for (const auto& pin : settings.state.pins)
        pins.push_back({{"name", pin.name}, {"kind", pin.kind}, {"revision", pin.revision}});
    disk::Row change{history_policy_schema, {history_format, static_cast<int64_t>(settings.state.floor),
        static_cast<int64_t>(settings.state.quota), pins.dump()}};
    if (settings.row) tx_.update(owner_, settings.row, change, disk::ChangeKind::catalog);
    else tx_.insert(owner_, change, disk::ChangeKind::catalog);
}

HistoryState HistoryJournal::state() { return verify(); }

HistoryState HistoryJournal::verify() { return visit(0, 0, {}); }

HistoryState HistoryJournal::visit(uint64_t first, uint64_t last, const Visitor& visitor)
{
    auto settings_snapshot = settings();
    auto& state = settings_snapshot.state;
    if ((first && first < state.floor) || (last && last < state.floor)) throw HistoryUnavailable(state.floor);
    if ((first && first > state.head) || last > state.head || (first && last && first > last))
        throw Error(Code::range, {}, "History revision range is outside the committed head");
    scan_history(engine_, state, first ? first : state.floor, last ? last : state.head, visitor, check_);
    tx_.ensure_active(owner_);
    return state;
}

HistoryRevision HistoryJournal::read(uint64_t revision)
{
    if (!revision) throw Error(Code::range, {}, "History revision number must be positive");
    std::optional<HistoryRevision> found;
    visit(revision, revision, [&](HistoryRevision&& change) { found.emplace(std::move(change)); });
    if (!found) throw Error(Code::range, {}, "History revision does not exist");
    return std::move(*found);
}

void HistoryJournal::seal(std::string_view actor, int64_t utc_seconds)
{
    bool only_outcomes{};
    tx_.visit_changes(owner_, [&](const auto changes) { only_outcomes = outcome_only(changes); });
    if (only_outcomes) return;
    if (!short_name(actor) || utc_seconds <= 0) throw Error(Code::type, {}, "Invalid History actor or commit time");
    auto settings_snapshot = settings();
    auto& state = settings_snapshot.state;
    constexpr auto maximum = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    if (state.head >= maximum) throw Error(Code::limit, {}, "History revision limit reached");
    auto previous = scan_history(engine_, state, state.floor, state.head, {}, check_);
    Seal pending{state.head + 1, state.head, std::string(actor), utc_seconds, previous, {}};
    tx_.visit_changes(owner_, [&](std::span<const disk::Change> changes) {
        if (std::ranges::any_of(changes, [](const disk::Change& change) { return schema(change) == history_seal_schema; }))
            corrupt("Transaction tried to append its History seal twice");
        const auto bytes = journal_bytes(changes);
        if (bytes && (state.retained_bytes >= state.quota || bytes > state.quota - state.retained_bytes))
            throw Error(Code::limit, {}, "Retained History quota reached");
        pending.hash = hash_changes(pending, changes, false);
    });
    tx_.insert(owner_, {history_seal_schema, {history_format, static_cast<int64_t>(pending.lsn),
        static_cast<int64_t>(pending.parent), pending.actor, pending.utc,
        disk::Bytes(pending.prior.begin(), pending.prior.end()), disk::Bytes(pending.hash.begin(), pending.hash.end())}},
        disk::ChangeKind::catalog);
}

void HistoryJournal::quota(uint64_t bytes)
{
    if (!bytes || bytes > max_quota) throw Error(Code::limit, {}, "Invalid History quota");
    auto current = settings();
    if (bytes == current.state.quota) return;
    current.state.quota = bytes;
    update_settings(current);
}

void HistoryJournal::pin(HistoryPin pin)
{
    if (!short_name(pin.name) || !pin_kind(pin.kind))
        throw Error(Code::type, {}, "Invalid History pin kind or name");
    auto current = settings();
    if (pin.revision < current.state.floor) throw HistoryUnavailable(current.state.floor);
    if (pin.revision > current.state.head) throw Error(Code::range, {}, "History pin revision is not committed");
    if (current.state.pins.size() >= max_pins) throw Error(Code::limit, {}, "History pin quota reached");
    if (std::ranges::any_of(current.state.pins, [&](const HistoryPin& item) {
        return item.kind == pin.kind && item.name == pin.name;
    }))
        throw Error(Code::name, {}, "History pin already exists");
    current.state.pins.push_back(std::move(pin));
    update_settings(current);
}

void HistoryJournal::unpin(std::string_view kind, std::string_view name)
{
    auto current = settings();
    if (!std::erase_if(current.state.pins, [&](const HistoryPin& item) {
        return item.kind == kind && item.name == name;
    }))
        throw Error(Code::name, {}, "Unknown History pin");
    update_settings(current);
}

void HistoryJournal::purge_before(uint64_t first_retained)
{
    auto current = settings();
    if (!first_retained || first_retained < current.state.floor || first_retained > current.state.head + 1)
        throw Error(Code::range, {}, "History floor cannot move backwards or beyond the next commit");
    if (first_retained == current.state.floor) return;
    for (const auto& pin : current.state.pins)
        if (pin.revision < first_retained) throw HistoryPinned(pin);
    current.state.floor = first_retained;
    update_settings(current);
}
}
