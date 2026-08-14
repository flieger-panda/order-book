// order_book.hpp
//
// A limit order book STORAGE LAYER: holds resting orders on the bid and ask
// sides, supports O(1) insertion at the back of a price level, O(1)
// cancellation by order id, and O(1)/O(log P) access to the best price on
// either side.
//
// ============================================================================
// DESIGN OVERVIEW
// ============================================================================
// This class is intentionally *just* storage. It has no notion of whether
// two orders should trade against each other -- that decision belongs to a
// MatchingEngine that sits on top and drives this class through
// addOrder()/cancelOrder(). Keeping that boundary clean means the removal
// logic (cancelOrder) only has to be written once: it's reused both for
// user-initiated cancels and internally by a matching engine whenever a
// resting order is fully consumed by a fill.
//
// Four structures compose the book:
//
//   1. Two std::map<Price, PriceLevel>          (bids desc, asks asc)
//        -- an ordered index from price -> the queue of orders resting at
//           that price. Ordering is what lets "find the best price" be
//           O(log P) (or O(1) with an array-of-ticks variant), where P is
//           the number of distinct occupied price levels -- NOT the number
//           of orders. That distinction is why this stays fast even under
//           very high order volume.
//
//   2. PriceLevel
//        -- head/tail of an intrusive doubly linked list of OrderNode, plus
//           a running aggregate quantity so depth queries don't require
//           walking the list.
//
//   3. OrderNode
//        -- one resting order. Doubles as a DLL node (prev/next) *and*
//           carries a back-pointer to its own PriceLevel, which is what
//           makes O(1) cancellation possible without already knowing the
//           order's price or side.
//
//   4. std::unordered_map<OrderId, OrderNode*>   (orderIndex)
//        -- a single global map spanning both sides and all prices. Given
//           only an order id, this is the O(1) entry point into the whole
//           structure -- cancel() never has to search a price table first.
//
// MEMORY OWNERSHIP: nothing above owns OrderNode memory except OrderPool.
// bids / asks / orderIndex / PriceLevel all hold *non-owning* raw pointers
// into nodes the pool allocated. This mirrors how a real low-latency engine
// avoids new/delete on the hot path -- cancels and fills are two of the
// most frequent operations in the whole system.
// ============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <deque>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>

namespace orderbook {

// ---------------------------------------------------------------------------
// Fixed-point price representation.
//
// Prices are stored as integer "ticks", not float/double. Floating point
// equality and ordering are unreliable for money (0.1 + 0.2 != 0.3 in IEEE
// 754), and a std::map's internal ordering invariants depend on a
// *consistent* comparator -- rounding error could silently corrupt which
// price level an order lands in. In a real system, tick = price * 10^N for
// whatever N the venue's tick size implies (e.g. cents, or 1/10000 for FX).
// ---------------------------------------------------------------------------
using Price    = int64_t;
using OrderId  = uint64_t;
using Quantity = int64_t;

enum class Side { Buy, Sell };

// Forward declaration so PriceLevel and OrderNode can reference each other.
struct PriceLevel;

// ---------------------------------------------------------------------------
// OrderNode: one resting order.
//
// This is deliberately an INTRUSIVE linked list node -- prev/next live
// directly on the order itself, rather than wrapping each order in a
// std::list<OrderNode>. Two reasons:
//   - std::list allocates a separate list-node object per element, doubling
//     allocations for an operation (insert/cancel) that happens on nearly
//     every request the book receives.
//   - intrusive nodes live together in OrderPool's contiguous storage,
//     which is friendlier to CPU cache behavior than nodes scattered
//     wherever a general-purpose allocator happened to put them.
//
// `level` is the back-pointer that makes O(1) cancellation possible: given
// only an OrderNode* (which is all orderIndex stores), you can find which
// PriceLevel to unlink from and update its aggregate, with no separate
// lookup by price.
//
// `side` exists for a similar reason one level up: cancelOrder() needs to
// know whether an emptied level should be erased from `bids` or `asks`,
// and PriceLevel itself doesn't record which of the two maps it lives in.
// ---------------------------------------------------------------------------
struct OrderNode {
    OrderId     id       = 0;
    Side        side     = Side::Buy;
    Quantity    quantity = 0;       // remaining quantity; shrinks on partial fills
    OrderNode*  prev     = nullptr;
    OrderNode*  next     = nullptr;
    PriceLevel* level    = nullptr; // which price level this order currently lives in
};

// ---------------------------------------------------------------------------
// PriceLevel: one row of the book -- a single price and everything resting
// there, in strict arrival order (price-time priority).
//
//   head = oldest order at this price = next in line to be matched.
//   tail = newest order = where a newly-arrived order at this price gets
//          appended.
//
// Both pointers are needed, not just one, because insertion happens at the
// tail while consumption happens at the head -- a single pointer could only
// make one of those two operations O(1).
//
// totalQuantity is a running aggregate rather than something recomputed by
// walking the list, because "how much size is resting at this price" is a
// question asked constantly (market data feeds, depth displays, a matching
// engine deciding how much of an incoming order it can fill) and shouldn't
// cost O(orders at this level) every time it's asked.
// ---------------------------------------------------------------------------
struct PriceLevel {
    Price       price         = 0;
    OrderNode*  head          = nullptr;
    OrderNode*  tail          = nullptr;
    Quantity    totalQuantity = 0;
    int         orderCount    = 0;
};

// ---------------------------------------------------------------------------
// OrderPool: owns every OrderNode's memory for the entire lifetime of the
// book.
//
// Nothing else in this file ever calls `new OrderNode` or `delete node`.
// bids, asks, and orderIndex all hold non-owning raw pointers into nodes
// this pool allocated -- the pool is the single source of truth for
// lifetime, which avoids the ownership ambiguity you'd get if, say, both
// the order index and a PriceLevel thought they were responsible for
// freeing the same node.
//
// Storage is a std::deque<OrderNode>, deliberately not std::vector.
// A vector reallocates and *moves* every existing element when it grows
// past capacity, which would invalidate every OrderNode* the rest of the
// book is currently holding -- a correctness bug that would only show up
// once the book got busy enough to trigger a reallocation. A deque grows
// by appending new fixed-size chunks: existing elements never move, so
// pointers/references into it stay valid for the deque's entire lifetime.
// That property is what makes it safe to freely hand out OrderNode* from
// acquire() and stash them in maps and linked lists elsewhere.
//
// freeList recycles released nodes instead of leaving them permanently
// "used up" in storage, so a long-running book doesn't grow storage
// forever just because orders keep arriving and cancelling.
// ---------------------------------------------------------------------------
class OrderPool {
public:
    OrderNode* acquire() {
        OrderNode* node;
        if (!freeList.empty()) {
            node = freeList.back();
            freeList.pop_back();
            *node = OrderNode{};       // reset to a clean default state before reuse
        } else {
            storage.emplace_back();    // deque growth never invalidates existing elements
            node = &storage.back();
        }
        return node;
    }

    void release(OrderNode* node) {
        freeList.push_back(node);
    }

private:
    std::deque<OrderNode>   storage;   // owns the actual memory; elements never move once placed
    std::vector<OrderNode*> freeList;  // recycled nodes available for reuse
};

// ---------------------------------------------------------------------------
// OrderBook: the storage layer described above, assembled into one class.
//
// This class answers exactly two kinds of question:
//   - "put this order at this price"        (addOrder)
//   - "remove this order id, wherever it is" (cancelOrder)
// plus read-only queries that let a MatchingEngine (or a test) see what
// it's working with, without reaching into private internals.
//
// It deliberately does NOT decide whether an incoming order should trade
// against the opposite side -- that's a MatchingEngine's job, built on top
// of this class: it calls addOrder() to rest unfilled quantity and
// cancelOrder() to remove resting orders that get fully consumed by a fill.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    OrderBook() = default;

    // Copying is deliberately disabled. bids/asks/orderIndex hold raw
    // pointers into `pool`'s internal storage -- a naive copy would copy
    // those pointer VALUES, leaving the copy's containers pointing at the
    // ORIGINAL book's nodes instead of its own. Move is fine: it transfers
    // ownership of pool's storage wholesale rather than duplicating it, so
    // pointers already handed out remain valid.
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = default;
    OrderBook& operator=(OrderBook&&)      = default;

    // Insert a brand-new resting order.
    //
    // Complexity: O(log P) if this price level does not exist yet (map
    // insertion creates a new tree node); O(1) if it does (list append +
    // hashmap insert only). P = number of distinct price levels currently
    // in the book, not number of orders.
    void addOrder(OrderId id, Side side, Price price, Quantity quantity) {
        if (orderIndex.find(id) != orderIndex.end()) {
            throw std::invalid_argument("addOrder: duplicate order id");
        }
        if (quantity <= 0) {
            throw std::invalid_argument("addOrder: quantity must be positive");
        }

        PriceLevel& level = findOrCreateLevel(side, price);

        OrderNode* node = pool.acquire();
        node->id       = id;
        node->side     = side;
        node->quantity = quantity;
        node->level    = &level;
        node->prev     = level.tail;
        node->next     = nullptr;

        // Append at the tail. This is what gives a newly-arrived order
        // correct time priority: it lines up BEHIND whatever already
        // rests at this exact price, never ahead of it.
        if (level.tail) {
            level.tail->next = node;
        } else {
            level.head = node;   // level was empty; this order is also the head now
        }
        level.tail = node;

        level.totalQuantity += quantity;
        level.orderCount++;

        orderIndex[id] = node;
    }

    // Remove an order by id, from wherever it currently lives.
    //
    // Complexity: O(1) for the unlink + index removal, plus O(log P) only
    // in the case where this cancel empties the price level and it has to
    // be erased from the sorted table (O(1) instead, with an array-of-ticks
    // implementation of bids/asks).
    //
    // Returns false rather than throwing if the id isn't currently resting
    // in the book (already filled, or already cancelled). Callers --
    // especially a matching engine racing fills against incoming cancels --
    // need to treat "already gone" as a normal, expected outcome rather
    // than an error condition.
    bool cancelOrder(OrderId id) {
        auto it = orderIndex.find(id);
        if (it == orderIndex.end()) {
            return false;                                        // 1. hashmap -> node   O(1)
        }

        OrderNode*  node  = it->second;
        PriceLevel* level = node->level;                          // 2. node -> its row   O(1)

        // 3. Unlink from the intrusive DLL. Each branch handles the "this
        //    node is the head/tail" edge case by repointing the PriceLevel
        //    itself instead of a (nonexistent) neighbor node.
        if (node->prev) node->prev->next = node->next; else level->head = node->next;
        if (node->next) node->next->prev = node->prev; else level->tail = node->prev;

        level->totalQuantity -= node->quantity;                    // 4. keep aggregate correct
        level->orderCount--;

        if (level->head == nullptr) {                              // 5. level is now empty --
            eraseLevel(node->side, level->price);                  //    drop the row from the table
        }

        orderIndex.erase(it);                                       // 6. drop from global index
        pool.release(node);                                          // 7. return memory to the pool
        return true;
    }

    // Reduce a resting order's quantity in place -- used when a fill only
    // PARTIALLY consumes it. Deliberately does NOT move the order within
    // its price level's queue: a partially-filled order keeps its original
    // time priority, which is standard exchange behavior (getting filled a
    // little shouldn't send you to the back of the line).
    //
    // If a fill fully consumes the order, callers should use cancelOrder()
    // instead -- a zero-quantity node has no reason to keep occupying a
    // slot in the book.
    bool reduceQuantity(OrderId id, Quantity amount) {
        auto it = orderIndex.find(id);
        if (it == orderIndex.end()) return false;

        OrderNode* node = it->second;
        if (amount <= 0 || amount > node->quantity) {
            throw std::invalid_argument("reduceQuantity: amount out of range");
        }

        node->quantity              -= amount;
        node->level->totalQuantity  -= amount;
        return true;
    }

    // --- Read-only queries --------------------------------------------------
    // These exist so a MatchingEngine (or a test) can inspect the book
    // without reaching into its private members. std::optional communicates
    // "this side is empty" without needing a magic sentinel price value.

    std::optional<Price> bestBid() const {
        if (bids.empty()) return std::nullopt;
        return bids.begin()->first;   // comparator is std::greater -> begin() = highest price
    }

    std::optional<Price> bestAsk() const {
        if (asks.empty()) return std::nullopt;
        return asks.begin()->first;   // default std::less -> begin() = lowest price
    }

    // Total resting quantity at a given price/side, or 0 if that price
    // level doesn't currently exist. O(log P).
    //
    // Written as if/else rather than a ternary on principle, not just
    // style: bids and asks are different template instantiations (see the
    // note above the member declarations), so `cond ? bids : asks` doesn't
    // compile -- there's no common type for the ternary to produce.
    Quantity quantityAt(Side side, Price price) const {
        if (side == Side::Buy) {
            auto it = bids.find(price);
            return (it != bids.end()) ? it->second.totalQuantity : 0;
        } else {
            auto it = asks.find(price);
            return (it != asks.end()) ? it->second.totalQuantity : 0;
        }
    }

    bool contains(OrderId id) const {
        return orderIndex.find(id) != orderIndex.end();
    }

    std::size_t orderCount() const {
        return orderIndex.size();
    }

    // Non-owning pointer to the oldest (head) order at the best price on a
    // side -- this is exactly the node a MatchingEngine would start walking
    // when matching an incoming order against resting liquidity. Returns
    // nullptr if that side currently has no resting orders.
    OrderNode* bestLevelHead(Side side) {
        if (side == Side::Buy) {
            if (bids.empty()) return nullptr;
            return bids.begin()->second.head;
        } else {
            if (asks.empty()) return nullptr;
            return asks.begin()->second.head;
        }
    }

    // Sum of resting quantity a hypothetical incoming order could trade
    // against, scanning from the best price outward across as many levels
    // as necessary -- stopping once either `cap` is reached or price levels
    // stop crossing.
    //
    // bestLevelHead() only ever exposes a single price level, which is all
    // the normal match loop needs (it re-queries after each level empties).
    // Fill-or-kill semantics are different: they need to know whether
    // *enough* liquidity exists across potentially several levels before
    // committing to any of it, which means looking past the best level
    // before a single trade happens. That lookahead has to live here rather
    // than in a MatchingEngine, because bids/asks are private -- this is
    // the query that lets one ask the question without reaching into
    // internals.
    //
    // `restingSide` is the side being scanned (the side opposite whatever
    // incoming order is asking); `limitPrice`/`isMarket` describe that
    // incoming order's own price. `cap` lets the caller stop early once it
    // has its answer instead of always walking the whole side.
    Quantity liquidityAvailable(Side restingSide, Price limitPrice, bool isMarket, Quantity cap) const {
        Quantity total = 0;
        if (restingSide == Side::Sell) {
            for (const auto& [price, level] : asks) {
                if (!isMarket && price > limitPrice) break;  // asks ascending: nothing further can cross either
                total += level.totalQuantity;
                if (total >= cap) break;
            }
        } else {
            for (const auto& [price, level] : bids) {
                if (!isMarket && price < limitPrice) break;  // bids descending: nothing further can cross either
                total += level.totalQuantity;
                if (total >= cap) break;
            }
        }
        return total;
    }

private:
    // findOrCreateLevel: the one place addOrder() touches the sorted
    // tables. std::map::operator[] default-constructs a PriceLevel the
    // first time a price is seen and returns the existing one otherwise --
    // exactly the "find, or create if absent" semantics addOrder() needs,
    // in a single call.
    PriceLevel& findOrCreateLevel(Side side, Price price) {
        PriceLevel& level = (side == Side::Buy) ? bids[price] : asks[price];
        level.price = price;   // no-op if the level already existed
        return level;
    }

    void eraseLevel(Side side, Price price) {
        if (side == Side::Buy) bids.erase(price);
        else                   asks.erase(price);
    }

    // -------------------------------------------------------------------
    // The four structures, assembled.
    //
    // `pool` is declared FIRST so it is constructed first and destroyed
    // LAST -- C++ destroys members in the REVERSE of their declaration
    // order. Since bids/asks/orderIndex only ever hold non-owning pointers
    // into nodes `pool` allocated, this ordering guarantees the pool's
    // memory is still valid for as long as anything else in the class
    // could reference it, right up through the rest of OrderBook's own
    // teardown.
    //
    // `bids` and `asks` are two DIFFERENT TYPES at compile time -- the
    // comparator is a template parameter baked into the type itself, not
    // a runtime flag on a shared type. That's why sort direction has to be
    // fixed via std::greater<Price> vs. the default std::less<Price>
    // rather than chosen dynamically.
    // -------------------------------------------------------------------
    OrderPool                                         pool;        // owns all OrderNode memory
    std::map<Price, PriceLevel, std::greater<Price>>  bids;        // highest price first
    std::map<Price, PriceLevel>                       asks;        // lowest price first
    std::unordered_map<OrderId, OrderNode*>           orderIndex;  // global, O(1) lookup by id
};

} // namespace orderbook
