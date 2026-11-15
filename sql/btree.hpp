#pragma once

#include "types.hpp"
#include "../storage/transactions.hpp"

namespace sdp::sql
{
using IndexKey = std::vector<disk::Value>;

struct IndexEntry
{
    IndexKey key;
    disk::RowId row{};
};

class BTree
{
public:
    BTree(disk::Transaction& transaction, disk::Owner owner, disk::RowId root, std::vector<Type> types,
        std::string_view collation = "binary");
    BTree(disk::Store& store, disk::RowId root, std::vector<Type> types, std::string_view collation = "binary");
    static disk::RowId create(disk::Transaction& transaction, disk::Owner owner);
    void insert(const IndexEntry& entry);
    void erase(const IndexEntry& entry);

    // Both key bounds are inclusive. The continuation uses an exclusive (key, RowId) boundary.
    bool visit(const std::optional<IndexKey>& lower, const std::optional<IndexKey>& upper,
        const std::optional<IndexEntry>& after, const std::function<bool(const IndexEntry&)>& visitor);
    std::vector<IndexEntry> equal(const IndexKey& key, size_t limit);
    std::vector<IndexEntry> range(const std::optional<IndexKey>& lower, const std::optional<IndexKey>& upper,
        size_t limit);

    void verify();
    // Erase all nodes, including the root. The surrounding transaction controls publication.
    void clear();
    // Retire a version-mismatched index using the verified hierarchy and exact fence records.
    void clear_stale();

private:
    struct Node;
    struct Promotion;

    void validate_definition() const;
    void validate_key(const IndexKey& key, bool measure = true);
    disk::Row read_row(disk::RowId id);
    disk::Transaction& writable();
    int compare_keys(const IndexKey& left, const IndexKey& right) const;
    bool less(const IndexEntry& left, const IndexEntry& right) const;
    size_t child_for(const Node& node, const IndexEntry& entry) const;
    Node read(disk::RowId id, bool verify_order = true);
    void write(disk::RowId id, const Node& node);
    disk::RowId allocate(const Node& node);
    std::optional<Promotion> insert_into(Node& node, const IndexEntry& entry, size_t depth);
    void erase_from(Node& node, const IndexEntry& entry, size_t depth);
    void rebalance(Node& parent, size_t child, Node node);
    std::vector<disk::RowId> verify_nodes(bool verify_rows, bool verify_order);
    static Node divide(Node& left);
    static disk::Row encode_node(const Node& node, disk::RowId owner_root);

    disk::Transaction* transaction_{};
    std::optional<disk::Editor> reader_;
    disk::Owner owner_{};
    disk::RowId root_;
    std::vector<Type> types_;
    std::string_view collation_;
};
}
