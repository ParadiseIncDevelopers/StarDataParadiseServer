#include "security.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace sdp::sql
{
namespace
{
constexpr uint32_t security_schema = 0x53444131;
constexpr uint64_t bootstrap_id = 1;
constexpr size_t max_users = 256;
constexpr size_t max_roles = 128;
constexpr size_t max_records = 4096;
constexpr std::array<std::string_view, 12> privileges{
    "usage", "select", "insert", "update", "delete", "ddl", "execute", "history", "backup", "debug",
    "monitor", "control"};
enum class Record : int64_t { user = 1, role, privilege, membership };

using AccountKey = uint64_t;
using Membership = std::pair<AccountKey, AccountKey>;
using Grant = std::tuple<AccountKey, ObjectId, std::string>;

struct Account
{
    Principal principal;
    disk::RowId row{};
    std::optional<server::Admin> verifier;
};

[[noreturn]] void corrupt()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid SQL security catalog");
}

[[noreturn]] void denied()
{
    throw wire::Error(wire::Code::permission_denied);
}

bool valid_name(std::string_view name)
{
    return !name.empty() && name.size() <= 64 &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == name.npos;
}

void require_name(std::string_view name, Source source)
{
    if (!valid_name(name)) throw Error(Code::name, source, "Invalid user or role name");
}

template<typename T>
const T& field(const disk::Row& row, size_t index)
{
    if (index >= row.values.size()) corrupt();
    const auto* value = std::get_if<T>(&row.values[index]);
    if (!value) corrupt();
    return *value;
}

template<size_t N>
disk::Bytes bytes(const std::array<unsigned char, N>& source)
{
    return {source.begin(), source.end()};
}

disk::Row user_record(std::string name, bool administrator, const std::optional<server::Admin>& secret)
{
    disk::Row row{security_schema, {static_cast<int64_t>(Record::user), std::move(name), int64_t{administrator},
        std::monostate{}, std::monostate{}}};
    if (secret)
    {
        row.values[3] = bytes(secret->salt);
        row.values[4] = bytes(secret->verifier);
    }
    return row;
}

std::optional<server::Admin> decode_verifier(const disk::Row& row, const std::string& name)
{
    const bool no_salt = std::holds_alternative<std::monostate>(row.values[3]);
    const bool no_verifier = std::holds_alternative<std::monostate>(row.values[4]);
    if (no_salt && no_verifier) return {};
    if (no_salt || no_verifier) corrupt();
    const auto& salt = field<disk::Bytes>(row, 3);
    const auto& digest = field<disk::Bytes>(row, 4);

    server::Admin verifier;
    verifier.username = name;
    if (salt.size() != verifier.salt.size() || digest.size() != verifier.verifier.size()) corrupt();
    std::ranges::copy(salt, verifier.salt.begin());
    std::ranges::copy(digest, verifier.verifier.begin());
    return verifier;
}

AccountKey subject_id(const disk::Row& row, size_t column)
{
    const auto value = field<int64_t>(row, column);
    if (value <= 0) corrupt();
    return static_cast<AccountKey>(value);
}

ObjectId scope(Catalog& catalog, const Statement& statement)
{
    const auto& kind = statement.security_kind;
    if (kind == "global") return 0;
    if (kind == "database") return catalog.resolve(ObjectKind::database, statement.target).id;
    if (kind == "schema") return catalog.resolve(ObjectKind::schema, statement.target).id;
    if (kind == "table") return catalog.resolve(ObjectKind::table, statement.target).id;
    if (kind == "view") return catalog.resolve(ObjectKind::view, statement.target).id;
    if (kind == "routine") return catalog.resolve(ObjectKind::routine, statement.target).id;
    throw Error(Code::unsupported, statement.source, "Unsupported permission scope");
}

template<class Function>
void statement_scope(disk::Transaction& transaction, disk::Owner owner, Function execute)
{
    const auto savepoint = transaction.statement_savepoint(owner);
    try
    {
        execute();
        transaction.release_savepoint(owner, savepoint);
    }
    catch (...)
    {
        try
        {
            transaction.rollback_to(owner, savepoint);
            transaction.release_savepoint(owner, savepoint);
        }
        catch (...)
        {
            try { transaction.rollback(owner); }
            catch (const disk::Error&) { /* The lease is already unavailable. */ }
        }
        throw;
    }
}

void validate_password(std::string_view password)
{
    if (password.size() < 16 || password.size() > 1024 || password.find('\0') != password.npos)
        throw Error(Code::type, {}, "Password must contain 16 to 1024 UTF-8 bytes");
    while (!password.empty())
    {
        const auto step = utf8_width(password);
        if (!step) throw Error(Code::type, {}, "Password must be valid UTF-8");
        password.remove_prefix(step);
    }
}
}

struct Security::Snapshot
{
    std::map<std::string, Account, std::less<>> accounts;
    std::map<std::string, AccountKey, std::less<>> roles;
    std::map<Membership, disk::RowId> memberships;
    std::map<Grant, disk::RowId> grants;

    size_t size() const { return accounts.size() + roles.size() + memberships.size() + grants.size(); }

    const Account* actor(const Principal& principal) const
    {
        const auto found = accounts.find(principal.name);
        return found != accounts.end() && found->second.principal.id == principal.id ? &found->second : nullptr;
    }
};

struct Security::Published
{
    std::shared_ptr<const Snapshot> snapshot;
    uint64_t lsn;
};

namespace
{
void read_record(Security::Snapshot& state, std::string_view bootstrap, disk::RowId id, const disk::Row& row)
{
    if (state.size() >= max_records) corrupt();
    switch (static_cast<Record>(field<int64_t>(row, 0)))
    {
    case Record::user:
    {
        if (row.values.size() != 5 || state.accounts.size() >= max_users) corrupt();
        const auto& name = field<std::string>(row, 1);
        const auto flag = field<int64_t>(row, 2);
        auto secret = decode_verifier(row, name);
        if (!valid_name(name) || (flag != 0 && flag != 1) || (flag != 0) != (name == bootstrap) ||
            (flag && !secret))
            corrupt();
        const auto user_id = flag ? bootstrap_id : id;
        if (!state.accounts.emplace(name, Account{{user_id, name, flag != 0}, id, std::move(secret)}).second)
            corrupt();
        break;
    }
    case Record::role:
    {
        if (row.values.size() != 2 || state.roles.size() >= max_roles) corrupt();
        const auto& name = field<std::string>(row, 1);
        if (!valid_name(name) || !state.roles.emplace(name, id).second) corrupt();
        break;
    }
    case Record::privilege:
    {
        if (row.values.size() != 4) corrupt();
        const auto target = field<int64_t>(row, 2);
        const auto& verb = field<std::string>(row, 3);
        if (target < 0 || std::ranges::find(privileges, verb) == privileges.end() ||
            !state.grants.emplace(Grant{subject_id(row, 1), target, verb}, id).second)
            corrupt();
        break;
    }
    case Record::membership:
        if (row.values.size() != 3 ||
            !state.memberships.emplace(Membership{subject_id(row, 1), subject_id(row, 2)}, id).second)
            corrupt();
        break;
    default:
        corrupt();
    }
}

void validate_references(const Security::Snapshot& state, std::string_view bootstrap)
{
    if (!state.accounts.contains(bootstrap)) corrupt();
    std::set<AccountKey> users;
    std::set<AccountKey> roles;
    for (const auto& [name, account] : state.accounts)
    {
        if (state.roles.contains(name) || !users.insert(account.principal.id).second) corrupt();
    }
    for (const auto& [name, id] : state.roles)
        if (users.contains(id) || !roles.insert(id).second) corrupt();
    for (const auto& [grant, row] : state.grants)
        if (!users.contains(std::get<0>(grant)) && !roles.contains(std::get<0>(grant))) corrupt();
    for (const auto& [member, row] : state.memberships)
        if (!users.contains(member.first) || !roles.contains(member.second)) corrupt();
}

void add_bootstrap(Security::Snapshot& state, const server::Admin& bootstrap)
{
    state.accounts.emplace(bootstrap.username, Account{{bootstrap_id, bootstrap.username, true}, 0, bootstrap});
}

disk::RowId seed_bootstrap(disk::Transaction& transaction, disk::Owner owner, const Security::Snapshot& state,
    const server::Admin& bootstrap)
{
    const auto& administrator = state.accounts.at(bootstrap.username);
    if (administrator.row) return administrator.row;
    return transaction.insert(owner, user_record(bootstrap.username, true, bootstrap), disk::ChangeKind::catalog);
}

void require_room(size_t current, size_t additions = 1)
{
    if (additions > max_records - current) throw Error(Code::limit, {}, "SQL security catalog quota reached");
}

AccountKey recipient(const Security::Snapshot& state, const Statement& statement)
{
    if (statement.security_role)
    {
        const auto found = state.roles.find(statement.security_subject);
        if (found != state.roles.end()) return found->second;
    }
    else
    {
        const auto found = state.accounts.find(statement.security_subject);
        if (found != state.accounts.end()) return found->second.principal.id;
    }
    throw Error(Code::name, statement.source, "Unknown permission recipient");
}

void change_membership(disk::Transaction& transaction, disk::Owner owner, const Security::Snapshot& state,
    const Statement& statement, AccountKey subject, bool grant)
{
    if (statement.security_role || statement.related.parts.size() != 1)
        throw Error(Code::unsupported, statement.source, "Role membership applies to users");
    const auto role = state.roles.find(statement.related.parts.front());
    if (role == state.roles.end()) throw Error(Code::name, statement.source, "Unknown role");
    const auto existing = state.memberships.find({subject, role->second});

    if (grant && existing == state.memberships.end())
    {
        require_room(state.size());
        transaction.insert(owner, {security_schema, {static_cast<int64_t>(Record::membership),
            static_cast<int64_t>(subject), static_cast<int64_t>(role->second)}}, disk::ChangeKind::catalog);
    }
    else if (!grant && existing != state.memberships.end())
        transaction.erase(owner, existing->second, disk::ChangeKind::catalog);
}

void change_grants(disk::Transaction& transaction, disk::Owner owner, const Security::Snapshot& state,
    const Statement& statement, AccountKey subject, bool grant)
{
    Catalog catalog(transaction, owner);
    const auto target = scope(catalog, statement);
    std::set<std::string_view> requested;
    for (const auto& verb : statement.permissions)
    {
        if (verb == "all") requested.insert(privileges.begin(), privileges.end());
        else
        {
            if (std::ranges::find(privileges, verb) == privileges.end())
                throw Error(Code::unsupported, statement.source, "Unsupported privilege");
            requested.insert(verb);
        }
    }
    size_t additions{};
    for (const auto verb : requested)
    {
        const auto existing = state.grants.find({subject, target, std::string(verb)});
        if (grant && existing == state.grants.end())
        {
            require_room(state.size(), ++additions);
            transaction.insert(owner, {security_schema, {static_cast<int64_t>(Record::privilege),
                static_cast<int64_t>(subject), target, std::string(verb)}}, disk::ChangeKind::catalog);
        }
        else if (!grant && existing != state.grants.end())
            transaction.erase(owner, existing->second, disk::ChangeKind::catalog);
    }
}
}

bool security_ddl(const Statement& statement)
{
    return statement.action == Action::create_user || statement.action == Action::create_role ||
        statement.action == Action::grant || statement.action == Action::revoke;
}

Security::Security(server::Admin bootstrap) : bootstrap_(std::move(bootstrap))
{
    if (!valid_name(bootstrap_.username)) corrupt();
    auto initial = std::make_shared<Snapshot>();
    add_bootstrap(*initial, bootstrap_);
    publish(std::move(initial), 0);
}

std::shared_ptr<const Security::Snapshot> Security::snapshot(disk::Transaction& transaction, disk::Owner owner) const
{
    auto state = std::make_shared<Snapshot>();
    transaction.visit(owner, [&](disk::RowId id, const disk::Row& row) {
        read_record(*state, bootstrap_.username, id, row);
    }, security_schema);
    if (!state->size()) add_bootstrap(*state, bootstrap_);
    validate_references(*state, bootstrap_.username);
    return state;
}

void Security::publish(std::shared_ptr<const Snapshot> state, uint64_t committed_lsn)
{
    if (!state) corrupt();
    auto next = std::make_shared<const Published>(Published{std::move(state), committed_lsn});
    const std::unique_lock held(publication_);
    if (!published_ || next->lsn >= published_->lsn) published_ = std::move(next);
}

uint64_t Security::commit_and_publish(std::shared_ptr<const Snapshot> state, const std::function<uint64_t()>& commit)
{
    if (!state) corrupt();
    auto next = std::make_shared<Published>(Published{std::move(state), 0});
    const std::unique_lock held(publication_);
    if (blocked_) throw disk::Error(disk::Fault::uncertain, "Security catalog requires recovery");
    uint64_t lsn;
    try { lsn = commit(); }
    catch (const disk::Error& error)
    {
        if (error.fault() == disk::Fault::uncertain) blocked_ = true;
        throw;
    }
    next->lsn = lsn;
    if (lsn >= published_->lsn) published_ = std::move(next);
    return lsn;
}

std::shared_ptr<const Security::Snapshot> Security::current() const
{
    const std::shared_lock held(publication_);
    return blocked_ ? std::shared_ptr<const Snapshot>{} : published_->snapshot;
}

std::optional<Principal> Security::authenticate(std::string_view user, std::string_view password) const
{
    const auto state = current();
    if (!state) return {};
    const auto found = state->accounts.find(user);
    if (found == state->accounts.end() || !found->second.verifier)
    {
        // Use the same KDF work without accepting an unknown or disabled account.
        bootstrap_.verify(user, password);
        return {};
    }
    if (!found->second.verifier->verify(user, password)) return {};
    return found->second.principal;
}

std::optional<Principal> Security::active_principal(std::string_view user) const
{
    const auto state = current();
    if (!state) return {};
    const auto found = state->accounts.find(user);
    if (found == state->accounts.end() || !found->second.verifier) return {};
    return found->second.principal;
}

bool Security::allowed(const Principal& actor, std::string_view verb, ObjectId target, ObjectId schema,
    ObjectId database) const
{
    const auto state = current();
    if (!state) return false;
    const auto* user = state->actor(actor);
    if (!user || !user->verifier) return false;
    if (user->principal.administrator) return true;

    const auto has_grant = [&](AccountKey who) {
        for (const auto scope : {target, schema, database, ObjectId{0}})
            if (state->grants.contains({who, scope, std::string(verb)})) return true;
        return false;
    };
    const auto id = user->principal.id;
    if (has_grant(id)) return true;
    for (auto position = state->memberships.lower_bound({id, 0});
        position != state->memberships.end() && position->first.first == id; ++position)
        if (has_grant(position->first.second)) return true;
    return false;
}

void Security::apply(disk::Transaction& transaction, disk::Owner owner, const Principal& actor,
    const Statement& statement)
{
    if (!security_ddl(statement)) throw Error(Code::unsupported, statement.source, "Expected security DDL");
    const auto state = snapshot(transaction, owner);
    const auto* user = state->actor(actor);
    if (!user || !user->principal.administrator) denied();

    statement_scope(transaction, owner, [&] {
        seed_bootstrap(transaction, owner, *state, bootstrap_);
        if (statement.action == Action::create_user || statement.action == Action::create_role)
        {
            if (statement.target.parts.size() != 1)
                throw Error(Code::name, statement.source, "User or role name must be unqualified");
            const auto& name = statement.target.parts.front();
            require_name(name, statement.target.source);
            if (state->accounts.contains(name) || state->roles.contains(name))
                throw Error(Code::name, statement.target.source, "User or role name already exists");

            require_room(state->size());
            if (statement.action == Action::create_user)
            {
                if (state->accounts.size() == max_users) throw Error(Code::limit, {}, "User quota reached");
                transaction.insert(owner, user_record(name, false, {}), disk::ChangeKind::catalog);
            }
            else
            {
                if (state->roles.size() == max_roles) throw Error(Code::limit, {}, "Role quota reached");
                transaction.insert(owner, {security_schema, {static_cast<int64_t>(Record::role), name}},
                    disk::ChangeKind::catalog);
            }
            return;
        }

        const auto subject = recipient(*state, statement);
        const bool grant = statement.action == Action::grant;
        if (statement.permissions.empty())
            change_membership(transaction, owner, *state, statement, subject, grant);
        else
            change_grants(transaction, owner, *state, statement, subject, grant);
    });
}

void Security::set_password(disk::Transaction& transaction, disk::Owner owner, const Principal& actor,
    std::string_view user, std::string_view old_password, std::string_view new_password)
{
    const auto state = snapshot(transaction, owner);
    const auto* requester = state->actor(actor);
    if (!requester || !requester->verifier || (!requester->principal.administrator && actor.name != user)) denied();
    const auto target = state->accounts.find(user);
    if (target == state->accounts.end()) throw Error(Code::name, {}, "Unknown password target");
    const auto& account = target->second;
    if ((!requester->principal.administrator || !old_password.empty()) &&
        (!account.verifier || !account.verifier->verify(user, old_password)))
        denied();
    validate_password(new_password);
    server::Admin updated;
    updated.username = account.principal.name;
    updated.set_password(new_password);

    statement_scope(transaction, owner, [&] {
        const auto bootstrap_row = seed_bootstrap(transaction, owner, *state, bootstrap_);
        const auto row = account.row ? account.row : bootstrap_row;
        transaction.update(owner, row, user_record(account.principal.name, account.principal.administrator, updated),
            disk::ChangeKind::catalog);
    });
}
}
