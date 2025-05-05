#pragma once

#include <tuple>
#include <utility>
#include <functional>
#include <cstddef>

// 自定义哈希函数,用于std::pair
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const noexcept {
        std::size_t h1 = std::hash<T1>{}(p.first);
        std::size_t h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// 自定义哈希函数,用于std::tuple(三元组)
struct triple_hash {
    template <class T1, class T2, class T3>
    std::size_t operator()(const std::tuple<T1, T2, T3>& t) const noexcept {
        std::size_t h1 = std::hash<T1>{}(std::get<0>(t));
        std::size_t h2 = std::hash<T2>{}(std::get<1>(t));
        std::size_t h3 = std::hash<T3>{}(std::get<2>(t));
        h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
        h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
        return h1;
    }
};
struct TupleHash {
    std::size_t operator()(const std::tuple<int, int, int>& t) const {
        auto h1 = std::hash<int>()(std::get<0>(t));
        auto h2 = std::hash<int>()(std::get<1>(t));
        auto h3 = std::hash<int>()(std::get<2>(t));
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};