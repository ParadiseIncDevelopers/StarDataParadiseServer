#include "btree.hpp"
#include "collation.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace sdp::sql
{
namespace
{
constexpr uint32_t node_schema = 0x53444931; // SDI1
constexpr disk::Byte node_format = 1;
constexpr size_t header_bytes = 24;
constexpr size_t maximum_keys = 16;
constexpr size_t minimum_keys = maximum_keys / 2;
constexpr size_t maximum_fields = 16;
constexpr size_t maximum_key_bytes = 2048;
constexpr size_t maximum_depth = 16;

[[noreturn]] void corrupt(const char* message)
{
    throw disk::Error(disk::Fault::corrupt, message);
}

bool valid_id(disk::RowId id)
{
    const auto page = id >> 32;
    const auto slot = static_cast<uint32_t>(id);
    return page != 0 && page < disk::page_limit && slot != 0 && slot <= UINT16_MAX;
}

void check_depth(size_t depth)
{
    if (depth >= maximum_depth) corrupt("Index depth limit exceeded");
}

template<class T>
void append_number(disk::Bytes& target, T value)
{
    const auto offset = target.size();
    target.resize(offset + sizeof(T));
    disk::put<T>(target, offset, value);
}

class Reader
{
public:
    explicit Reader(std::span<const disk::Byte> bytes) : bytes_(bytes) {}

    std::span<const disk::Byte> take(size_t size)
    {
        if (size > bytes_.size()) corrupt("Index node record is truncated");
        const auto result = bytes_.first(size);
        bytes_ = bytes_.subspan(size);
        return result;
    }

    template<class T>
    T number()
    {
        return disk::load_le<T>(take(sizeof(T)), 0);
    }

    bool done() const noexcept { return bytes_.empty(); }

private:
    std::span<const disk::Byte> bytes_;
};
}

struct BTree::Node
{
    bool leaf = true;
    disk::RowId next{};
    // Internal nodes keep one fence key per child, including the first child.
    std::vector<IndexEntry> entries;
    std::vector<disk::RowId> children;
};

struct BTree::Promotion
{
    IndexEntry first;
    disk::RowId id;
};

BTree::BTree(disk::Transaction& transaction, disk::Owner owner, disk::RowId root, std::vector<Type> types,
    std::string_view collation)
    : transaction_(&transaction), owner_(owner), root_(root), types_(std::move(types)),
      collation_(canonical_collation(collation))
{
    validate_definition();
}

BTree::BTree(disk::Store& store, disk::RowId root, std::vector<Type> types, std::string_view collation)
    : reader_(std::in_place, store), root_(root), types_(std::move(types)), collation_(canonical_collation(collation))
{
    validate_definition();
}

void BTree::validate_definition() const
{
    if (!valid_id(root_)) corrupt("Invalid index root identity");
    if (types_.empty() || types_.size() > maximum_fields)
        throw Error(Code::limit, {}, "An index requires between 1 and 16 key columns");
    for (const auto type : types_) validate_type(type);
    if (collation_ != "binary" && !std::ranges::any_of(types_, [](Type type) { return type.kind == Kind::text; }))
        throw Error(Code::type, {}, "A locale collation requires a TEXT index column");
}

disk::RowId BTree::create(disk::Transaction& transaction, disk::Owner owner)
{
    return transaction.insert(owner, encode_node({}, 0));
}

void BTree::validate_key(const IndexKey& key, bool measure)
{
    if (key.size() != types_.size()) throw Error(Code::type, {}, "Index key width does not match its definition");
    for (size_t i = 0; i != key.size(); ++i)
        if (coerce(key[i], types_[i], types_[i]) != key[i])
            throw Error(Code::type, {}, "Index key is not stored in canonical form");
    if (measure && disk::encode({1, key}).size() > maximum_key_bytes)
        throw Error(Code::limit, {}, "Index key exceeds 2048 encoded bytes");
}

int BTree::compare_keys(const IndexKey& left, const IndexKey& right) const
{
    for (size_t i = 0; i != types_.size(); ++i)
    {
        const auto left_null = std::holds_alternative<std::monostate>(left[i]);
        const auto right_null = std::holds_alternative<std::monostate>(right[i]);
        if (left_null != right_null) return left_null ? -1 : 1;
        if (left_null) continue;
        const auto type = types_[i];
        const auto order = type.kind == Kind::text && collation_ != "binary"
            ? compare_collated(std::get<std::string>(left[i]), std::get<std::string>(right[i]), collation_)
            : compare_non_null(left[i], type, right[i], type);
        if (order) return order;
    }
    return 0;
}

bool BTree::less(const IndexEntry& left, const IndexEntry& right) const
{
    const auto order = compare_keys(left.key, right.key);
    return order != 0 ? order < 0 : left.row < right.row;
}

size_t BTree::child_for(const Node& node, const IndexEntry& entry) const
{
    const auto child = std::ranges::upper_bound(node.entries, entry,
        [this](const auto& left, const auto& right) { return less(left, right); });
    return child == node.entries.begin() ? 0 : static_cast<size_t>(child - node.entries.begin() - 1);
}

disk::Row BTree::encode_node(const Node& node, disk::RowId owner_root)
{
    disk::Bytes bytes(header_bytes);
    bytes[0] = node_format;
    bytes[1] = static_cast<disk::Byte>(node.leaf);
    disk::put<uint16_t>(bytes, 2, static_cast<uint16_t>(node.entries.size()));
    disk::put<disk::RowId>(bytes, 8, owner_root);
    disk::put<disk::RowId>(bytes, 16, node.next);
    for (size_t i = 0; i != node.entries.size(); ++i)
    {
        const auto& entry = node.entries[i];
        const auto key = disk::encode({1, entry.key});
        append_number(bytes, entry.row);
        if (!node.leaf) append_number(bytes, node.children[i]);
        append_number(bytes, static_cast<uint32_t>(key.size()));
        bytes.insert(bytes.end(), key.begin(), key.end());
    }
    return {node_schema, {std::move(bytes)}};
}

disk::Row BTree::read_row(disk::RowId id)
{
    return transaction_ ? transaction_->get(owner_, id) : reader_->get(id);
}

disk::Transaction& BTree::writable()
{
    if (!transaction_) throw disk::Error(disk::Fault::invalid, "Index is open read-only");
    return *transaction_;
}

BTree::Node BTree::read(disk::RowId id, bool verify_order)
{
    if (!valid_id(id)) corrupt("Invalid index node identity");
    disk::Row row;
    try { row = read_row(id); }
    catch (const disk::Error& error)
    {
        if (error.fault() == disk::Fault::missing) corrupt("Index node is missing");
        throw;
    }
    if (row.schema != node_schema || row.values.size() != 1)
        corrupt("Index node does not reference an index record");
    const auto* bytes = std::get_if<disk::Bytes>(&row.values.front());
    if (!bytes) corrupt("Index node payload is invalid");
    Reader input(*bytes);
    if (input.number<disk::Byte>() != node_format) corrupt("Unknown index node format");
    const auto leaf = input.number<disk::Byte>();
    const auto count = input.number<uint16_t>();
    if (leaf > 1 || count > maximum_keys || input.number<uint32_t>() != 0)
        corrupt("Invalid index node header");
    Node node;
    node.leaf = leaf != 0;
    const auto root = input.number<disk::RowId>();
    node.next = input.number<disk::RowId>();
    if (root != (id == root_ ? 0 : root_) || (!node.leaf && node.next) ||
        (node.next && (!valid_id(node.next) || node.next == id || node.next == root_)))
        corrupt("Index node owner or leaf link is invalid");
    if (id == root_)
    {
        if (node.next || (!node.leaf && count < 2)) corrupt("Index root has an invalid shape");
    }
    else if (count < minimum_keys) corrupt("Index node occupancy is below its minimum");
    for (uint16_t ordinal = 0; ordinal < count; ++ordinal)
    {
        const auto row_id = input.number<disk::RowId>();
        if (!valid_id(row_id)) corrupt("Invalid index target row identity");
        if (!node.leaf)
        {
            const auto child = input.number<disk::RowId>();
            if (!valid_id(child) || child == id || child == root_) corrupt("Invalid index child identity");
            node.children.push_back(child);
        }
        const auto length = input.number<uint32_t>();
        if (!length || length > maximum_key_bytes) corrupt("Invalid index key length");
        auto key = disk::decode(input.take(length));
        if (key.schema != 1) corrupt("Invalid encoded index key format");
        try { validate_key(key.values, false); }
        catch (const Error&) { corrupt("Index key does not match its stored type"); }
        IndexEntry entry{std::move(key.values), row_id};
        if (verify_order && !node.entries.empty() && !less(node.entries.back(), entry))
            corrupt("Index keys are out of order");
        node.entries.push_back(std::move(entry));
    }
    if (!input.done()) corrupt("Index node record has trailing bytes");
    return node;
}

void BTree::write(disk::RowId id, const Node& node)
{
    writable().update(owner_, id, encode_node(node, id == root_ ? 0 : root_));
}

disk::RowId BTree::allocate(const Node& node)
{
    return writable().insert(owner_, encode_node(node, root_));
}

BTree::Node BTree::divide(Node& left)
{
    Node right;
    right.leaf = left.leaf;
    right.next = left.next;
    const auto middle = left.entries.size() / 2;
    std::move(left.entries.begin() + middle, left.entries.end(), std::back_inserter(right.entries));
    left.entries.resize(middle);
    if (!left.leaf)
    {
        right.children.assign(left.children.begin() + middle, left.children.end());
        left.children.resize(middle);
    }
    return right;
}

std::optional<BTree::Promotion> BTree::insert_into(Node& node, const IndexEntry& entry, size_t depth)
{
    check_depth(depth);
    const auto order = [this](const auto& left, const auto& right) { return less(left, right); };
    if (node.leaf)
    {
        const auto at = std::ranges::lower_bound(node.entries, entry, order);
        if (at != node.entries.end() && !less(entry, *at))
            throw Error(Code::constraint, {}, "Index entry already exists");
        node.entries.insert(at, entry);
    }
    else
    {
        const auto index = child_for(node, entry);
        const auto id = node.children[index];
        auto child = read(id);
        auto promoted = insert_into(child, entry, depth + 1);
        node.entries[index] = child.entries.front();
        write(id, child);
        if (promoted)
        {
            node.entries.insert(node.entries.begin() + index + 1, std::move(promoted->first));
            node.children.insert(node.children.begin() + index + 1, promoted->id);
        }
    }
    if (node.entries.size() <= maximum_keys) return std::nullopt;
    auto right = divide(node);
    const auto right_id = allocate(right);
    if (node.leaf) node.next = right_id;
    return Promotion{std::move(right.entries.front()), right_id};
}

void BTree::insert(const IndexEntry& entry)
{
    validate_key(entry.key);
    if (!valid_id(entry.row)) throw Error(Code::type, {}, "Invalid index row identity");
    auto node = read(root_);
    auto split = insert_into(node, entry, 0);
    if (split)
    {
        const auto left_id = allocate(node);
        Node parent{false, 0, {std::move(node.entries.front()), std::move(split->first)}, {left_id, split->id}};
        write(root_, parent);
        return;
    }
    write(root_, node);
}

void BTree::rebalance(Node& parent, size_t index, Node node)
{
    const auto left_index = index == 0 ? 0 : index - 1;
    const auto right_index = left_index + 1;
    const auto left_id = parent.children.at(left_index);
    const auto right_id = parent.children.at(right_index);
    auto left = index == left_index ? std::move(node) : read(left_id);
    auto right = index == right_index ? std::move(node) : read(right_id);
    if (left.leaf != right.leaf || (left.leaf && left.next != right_id) ||
        (!left.entries.empty() && !right.entries.empty() && !less(left.entries.back(), right.entries.front())))
        corrupt("Index sibling records disagree");

    std::move(right.entries.begin(), right.entries.end(), std::back_inserter(left.entries));
    left.children.insert(left.children.end(), right.children.begin(), right.children.end());
    left.next = right.next;
    if (left.entries.size() <= maximum_keys)
    {
        parent.entries.erase(parent.entries.begin() + right_index);
        parent.children.erase(parent.children.begin() + right_index);
        writable().erase(owner_, right_id);
    }
    else
    {
        auto redistributed = divide(left);
        if (left.leaf) left.next = right_id;
        parent.entries[right_index] = redistributed.entries.front();
        write(right_id, redistributed);
    }
    parent.entries[left_index] = left.entries.front();
    write(left_id, left);
}

void BTree::erase_from(Node& node, const IndexEntry& entry, size_t depth)
{
    check_depth(depth);
    if (node.leaf)
    {
        const auto at = std::ranges::lower_bound(node.entries, entry,
            [this](const auto& left, const auto& right) { return less(left, right); });
        if (at == node.entries.end() || less(entry, *at)) throw disk::Error(disk::Fault::missing, "Index entry is missing");
        node.entries.erase(at);
        return;
    }
    const auto index = child_for(node, entry);
    const auto id = node.children[index];
    auto child = read(id);
    erase_from(child, entry, depth + 1);
    if (child.entries.size() < minimum_keys)
        rebalance(node, index, std::move(child));
    else
    {
        node.entries[index] = child.entries.front();
        write(id, child);
    }
}

void BTree::erase(const IndexEntry& entry)
{
    validate_key(entry.key);
    if (!valid_id(entry.row)) throw Error(Code::type, {}, "Invalid index row identity");
    auto node = read(root_);
    erase_from(node, entry, 0);
    if (!node.leaf && node.children.size() == 1)
    {
        const auto only_id = node.children.front();
        write(root_, read(only_id));
        writable().erase(owner_, only_id);
    }
    else write(root_, node);
}

bool BTree::visit(const std::optional<IndexKey>& lower, const std::optional<IndexKey>& upper,
    const std::optional<IndexEntry>& after, const std::function<bool(const IndexEntry&)>& visitor)
{
    if (lower) validate_key(*lower);
    if (upper) validate_key(*upper);
    if (after)
    {
        validate_key(after->key);
        if (!valid_id(after->row)) throw Error(Code::type, {}, "Invalid index continuation identity");
    }
    if (lower && upper && compare_keys(*lower, *upper) > 0) return true;

    auto seek = after;
    bool exclusive = seek.has_value();
    if (lower && (!seek || compare_keys(*lower, seek->key) > 0))
    {
        seek = IndexEntry{*lower, 0};
        exclusive = false;
    }
    auto id = root_;
    auto node = read(id);
    for (size_t depth = 1; !node.leaf; ++depth)
    {
        check_depth(depth);
        id = node.children[seek ? child_for(node, *seek) : 0];
        node = read(id);
    }

    const auto order = [this](const auto& left, const auto& right) { return less(left, right); };
    auto first = node.entries.cbegin();
    if (seek)
        first = exclusive ? std::ranges::upper_bound(node.entries, *seek, order)
                          : std::ranges::lower_bound(node.entries, *seek, order);
    auto position = static_cast<size_t>(first - node.entries.cbegin());
    for (;;)
    {
        for (; position < node.entries.size(); ++position)
        {
            const auto& entry = node.entries[position];
            if (upper && compare_keys(entry.key, *upper) > 0) return true;
            if (!visitor(entry)) return false;
        }
        if (!node.next) return true;
        auto next = read(node.next);
        if (!next.leaf || !less(node.entries.back(), next.entries.front())) corrupt("Index leaf chain is out of order");
        node = std::move(next);
        position = 0;
    }
}

std::vector<IndexEntry> BTree::range(const std::optional<IndexKey>& lower, const std::optional<IndexKey>& upper, size_t limit)
{
    std::vector<IndexEntry> rows;
    if (limit) visit(lower, upper, {}, [&](const IndexEntry& row) {
        rows.push_back(row);
        return rows.size() < limit;
    });
    return rows;
}

std::vector<IndexEntry> BTree::equal(const IndexKey& key, size_t limit)
{
    return range(key, key, limit);
}

std::vector<disk::RowId> BTree::verify_nodes(bool verify_rows, bool verify_order)
{
    std::vector<disk::RowId> postorder;
    std::set<disk::RowId> visited;
    std::set<disk::RowId> rows;
    std::optional<size_t> leaf_depth;
    disk::RowId expected_next{};
    std::optional<IndexEntry> previous;
    const auto walk = [&](const auto& descend, disk::RowId id, size_t depth) -> std::optional<IndexEntry> {
        check_depth(depth);
        if (!visited.insert(id).second) corrupt("Index node is reachable more than once");
        const auto node = read(id, verify_order);
        if (node.leaf)
        {
            if (leaf_depth && (*leaf_depth != depth || expected_next != id))
                corrupt("Index leaf chain and index hierarchy disagree");
            leaf_depth = depth;
            expected_next = node.next;
            for (const auto& entry : node.entries)
            {
                if (verify_order && previous && !less(*previous, entry)) corrupt("Index leaf keys overlap");
                if (verify_rows)
                {
                    if (!rows.insert(entry.row).second) corrupt("Index row appears with multiple keys");
                    try
                    {
                        if (read_row(entry.row).schema == node_schema)
                            corrupt("Index target is an index node");
                    }
                    catch (const disk::Error& error)
                    {
                        if (error.fault() == disk::Fault::missing) corrupt("Index row is missing");
                        throw;
                    }
                }
                if (verify_order) previous = entry;
            }
        }
        else
        {
            for (size_t i = 0; i != node.children.size(); ++i)
            {
                const auto minimum = descend(descend, node.children[i], depth + 1);
                if (!minimum || node.entries[i].row != minimum->row || node.entries[i].key != minimum->key)
                    corrupt("Index fence key does not match its child");
            }
        }
        postorder.push_back(id);
        if (node.entries.empty()) return {};
        return node.entries.front();
    };
    walk(walk, root_, 0);
    if (expected_next) corrupt("Index leaf chain continues beyond its final child");
    return postorder;
}

void BTree::verify()
{
    verify_nodes(true, true);
}

void BTree::clear()
{
    auto& transaction = writable();
    for (const auto id : verify_nodes(false, true)) transaction.erase(owner_, id);
}

void BTree::clear_stale()
{
    auto& transaction = writable();
    for (const auto id : verify_nodes(false, false)) transaction.erase(owner_, id);
}
}
