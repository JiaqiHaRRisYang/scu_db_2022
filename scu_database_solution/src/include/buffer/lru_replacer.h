/**
 * lru_replacer.h
 *
 * Functionality: The buffer pool manager must maintain a LRU list to collect
 * all the pages that are unpinned and ready to be swapped. The simplest way to
 * implement LRU is a FIFO queue, but remember to dequeue or enqueue pages when
 * a page changes from unpinned to pinned, or vice-versa.
 */

#pragma once

#include "buffer/replacer.h"
#include "hash/extendible_hash.h"

#include <memory>
#include <mutex>
#include <unordered_map>
namespace scudb {

    template<typename T>
    class LRUReplacer : public Replacer<T> {
        struct Node {
            //contructor
            Node() {};
            Node(T val){
                this->val = val;
            }

            T val;
            std::shared_ptr<Node> prev;
            std::shared_ptr<Node> next;
        };
    public:
        // do not change public interface
        LRUReplacer();

        ~LRUReplacer();

        void Insert(const T &value);

        bool Victim(T &value);

        bool Erase(const T &value);

        size_t Size();

    private:
        // add your member variables here
        std::shared_ptr<Node> head;
        std::shared_ptr<Node> tail;
        unordered_map<T, shared_ptr<Node>> map;
        mutable mutex latch;
    };

} // namespace scudb
