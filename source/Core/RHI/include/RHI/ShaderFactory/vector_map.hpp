#pragma once
#include <algorithm>
#include <stdexcept>
#include <vector>

template<typename Key, typename Value>
class VectorMap {
   public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<Key, Value>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;
    using size_type = typename std::vector<value_type>::size_type;

    VectorMap() = default;

    // Iterator access
    iterator begin()
    {
        return data_.begin();
    }
    const_iterator begin() const
    {
        return data_.begin();
    }
    iterator end()
    {
        return data_.end();
    }
    const_iterator end() const
    {
        return data_.end();
    }

    // Capacity
    bool empty() const
    {
        return data_.empty();
    }
    size_type size() const
    {
        return data_.size();
    }

    // Element access
    Value& operator[](const Key& key)
    {
        auto it = find(key);
        if (it != end()) {
            return it->second;
        }
        data_.emplace_back(key, Value{});
        return data_.back().second;
    }

    Value& at(const Key& key)
    {
        auto it = find(key);
        if (it == end()) {
            throw std::out_of_range("VectorMap::at");
        }
        return it->second;
    }

    const Value& at(const Key& key) const
    {
        auto it = find(key);
        if (it == end()) {
            throw std::out_of_range("VectorMap::at");
        }
        return it->second;
    }

    // Modifiers
    std::pair<iterator, bool> insert(const value_type& value)
    {
        auto it = find(value.first);
        if (it != end()) {
            return { it, false };
        }
        data_.push_back(value);
        return { data_.end() - 1, true };
    }

    std::pair<iterator, bool> emplace(Key&& key, Value&& value)
    {
        auto it = find(key);
        if (it != end()) {
            return { it, false };
        }
        data_.emplace_back(std::forward<Key>(key), std::forward<Value>(value));
        return { data_.end() - 1, true };
    }

    iterator erase(const_iterator pos)
    {
        return data_.erase(pos);
    }

    size_type erase(const Key& key)
    {
        auto it = find(key);
        if (it != end()) {
            data_.erase(it);
            return 1;
        }
        return 0;
    }

    void clear()
    {
        data_.clear();
    }

    // Lookup
    iterator find(const Key& key)
    {
        return std::find_if(
            data_.begin(), data_.end(), [&key](const value_type& pair) {
                return pair.first == key;
            });
    }

    const_iterator find(const Key& key) const
    {
        return std::find_if(
            data_.begin(), data_.end(), [&key](const value_type& pair) {
                return pair.first == key;
            });
    }

    size_type count(const Key& key) const
    {
        return find(key) != end() ? 1 : 0;
    }

   private:
    std::vector<value_type> data_;
};
