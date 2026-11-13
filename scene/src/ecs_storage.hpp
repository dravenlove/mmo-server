#pragma once

#include <entt/entt.hpp>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace mmo {

// 排序索引只持有实体句柄。所有权威数据直接存储在 registry 的稳定组件池中，
// 既有系统通过此视图访问同一份组件，不另建逐 Tick 的 map/DTO 镜像。
// 保留按外部 ID 排序的迭代顺序，以维持 AOI、首次碰撞及旧 JSON 顺序。
template<class Component, class Category>
class EcsStorage {
    using Index = std::map<std::string, entt::entity>;
    entt::registry* registry_;
    Index index_;

public:
    explicit EcsStorage(entt::registry& registry) : registry_(&registry) {
        static_assert(entt::component_traits<Component>::in_place_delete,
                      "scene components must preserve addresses while other entities change");
    }
    EcsStorage(const EcsStorage&) = delete;
    EcsStorage& operator=(const EcsStorage&) = delete;

    template<bool Constant>
    class Iterator {
        using Storage = std::conditional_t<Constant, const EcsStorage, EcsStorage>;
        using Position = std::conditional_t<Constant, typename Index::const_iterator, typename Index::iterator>;
        using Value = std::conditional_t<Constant, const Component, Component>;
        Storage* owner_ = nullptr;
        Position position_{};
        struct Entry { const std::string& first; Value& second; };
        mutable std::optional<Entry> entry_;
        friend class EcsStorage;
        Iterator(Storage* owner, Position position) : owner_(owner), position_(position) {}

    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = Entry;
        using reference = Entry&;
        using pointer = Entry*;
        Iterator() = default;
        Iterator(const Iterator& other) : owner_(other.owner_), position_(other.position_) {}
        Iterator& operator=(const Iterator& other) {
            if (this != &other) { owner_ = other.owner_; position_ = other.position_; entry_.reset(); }
            return *this;
        }
        reference operator*() const {
            if (!entry_) {
                if constexpr (Constant) {
                    const entt::registry& registry = *owner_->registry_;
                    entry_.emplace(Entry{position_->first, registry.template get<Component>(position_->second)});
                } else {
                    entry_.emplace(Entry{position_->first, owner_->registry_->template get<Component>(position_->second)});
                }
            }
            return *entry_;
        }
        pointer operator->() const { return &operator*(); }
        Iterator& operator++() { ++position_; entry_.reset(); return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        bool operator==(const Iterator& other) const {
            return owner_ == other.owner_ && (owner_ == nullptr || position_ == other.position_);
        }
        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    iterator begin() { return iterator(this, index_.begin()); }
    iterator end() { return iterator(this, index_.end()); }
    const_iterator begin() const { return const_iterator(this, index_.begin()); }
    const_iterator end() const { return const_iterator(this, index_.end()); }
    iterator find(const std::string& id) { return iterator(this, index_.find(id)); }
    const_iterator find(const std::string& id) const { return const_iterator(this, index_.find(id)); }
    std::size_t size() const { return index_.size(); }
    bool empty() const { return index_.empty(); }
    std::size_t count(const std::string& id) const { return index_.count(id); }
    Component& at(const std::string& id) { return registry_->template get<Component>(index_.at(id)); }
    const Component& at(const std::string& id) const {
        const entt::registry& registry = *registry_;
        return registry.template get<Component>(index_.at(id));
    }
    template<class Value>
    std::pair<iterator, bool> emplace(const std::string& id, Value&& value) {
        // 调用方可能传 value.id 和 std::move(value)，必须先保存键再移动组件。
        const std::string key = id;
        auto found = index_.find(key);
        if (found != index_.end()) return {iterator(this, found), false};
        const auto handle = registry_->create();
        try {
            registry_->template emplace<Component>(handle, std::forward<Value>(value));
            registry_->template emplace<Category>(handle);
            auto inserted = index_.emplace(key, handle);
            return {iterator(this, inserted.first), true};
        } catch (...) {
            registry_->destroy(handle);
            throw;
        }
    }
    template<class Value>
    std::pair<iterator, bool> insert_or_assign(const std::string& id, Value&& value) {
        auto found = index_.find(id);
        if (found == index_.end()) return emplace(id, std::forward<Value>(value));
        registry_->template get<Component>(found->second) = std::forward<Value>(value);
        return {iterator(this, found), false};
    }
    iterator erase(iterator position) {
        const auto handle = position.position_->second;
        auto next = index_.erase(position.position_);
        registry_->destroy(handle);
        return iterator(this, next);
    }
    std::size_t erase(const std::string& id) {
        auto found = find(id);
        if (found == end()) return 0;
        erase(found);
        return 1;
    }
    void clear() {
        for (const auto& item : index_) registry_->destroy(item.second);
        index_.clear();
    }
    std::size_t component_count() const {
        const entt::registry& registry = *registry_;
        std::size_t result = 0;
        for (auto handle : registry.template view<const Component, const Category>()) { (void)handle; ++result; }
        return result;
    }
};
}  // namespace mmo
