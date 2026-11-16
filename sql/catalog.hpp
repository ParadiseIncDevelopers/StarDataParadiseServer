#pragma once

#include "syntax.hpp"
#include "../storage/transactions.hpp"

namespace sdp::sql
{
inline constexpr std::string_view default_database = "main";
inline constexpr std::string_view default_schema = "public";

enum class ObjectKind { database = 1, schema, table, index, view, routine, sequence, decision, model };
using ObjectId = int64_t;

struct Column
{
    std::string name;
    Type type;
    bool nullable;
    uint32_t ordinal;
};

struct Object
{
    ObjectId id{};
    ObjectId parent{};
    ObjectKind kind{};
    std::string name;
    uint32_t version{1};
    std::vector<Column> columns;
    std::string definition;
};

class Catalog
{
public:
    static std::optional<Object> history_object(disk::RowId id, const disk::Row& row);
    Catalog(disk::Transaction& transaction, disk::Owner owner, std::function<void()> check = {})
        : transaction_(&transaction), owner_(owner), check_(std::move(check)) {}
    explicit Catalog(disk::Store& store) : store_(&store) {}

    std::vector<Object> list() const;
    Object resolve(ObjectKind kind, const Name& name) const;
    Object resolve_relation(const Name& name) const;
    Object validate_auxiliary(ObjectKind kind, const Name& name) const;
    void update_definition(const Object& object, std::string definition, bool revise = false);
    void replace_table(const Object& before, const Object& after, Source source);
    void remove(const Object& object);
    void restore_object(const Object& object);
    void rename(const Object& object, const Name& new_name);
    Name qualified(const Object& object) const;
    // Reuse an ordered snapshot returned by list() when qualifying several objects.
    static Name qualified(const Object& object, std::span<const Object> snapshot);
    Object validate(const Statement& statement) const;
    Object apply(const Statement& statement);
    Object create_auxiliary(ObjectKind kind, const Name& name, std::string definition);

private:
    disk::Transaction* transaction_{};
    disk::Store* store_{};
    disk::Owner owner_{};
    std::function<void()> check_;
};
}
