// matching_engine.hpp
//
// Sits on top of OrderBook and decides the one thing OrderBook deliberately
// does not: whether an incoming order should trade against resting
// liquidity before any of it is allowed to rest.
//
// ============================================================================
// DESIGN OVERVIEW
// ============================================================================
// MatchingEngine owns no order storage of its own -- it holds a reference to
// an existing OrderBook and drives it entirely through the public interface
// already described in order_book.hpp: bestLevelHead() to find what to
// trade against, reduceQuantity()/cancelOrder() to consume it, and
// addOrder() to rest whatever's left over. That boundary is what lets
// OrderBook stay pure storage -- every notion of "should these two orders
// trade" lives here, and nowhere in order_book.hpp.
//
// One public entry point, submit(), runs the same loop for every order:
//
//   while there's quantity left AND the incoming order still crosses the
//   best resting price on the other side:
//       take a bite out of the oldest resting order at that price
//       record a trade at the RESTING order's price (it was there first)
//       shrink or remove that resting order depending on how much it had
//
// Order type (Limit vs Market) only changes what "crosses" means. Time in
// force (GTC vs IOC vs FOK) and postOnly only change what happens to
// whatever's left over once the loop stops -- rest it, drop it, or in FOK's
// case, refuse to run the loop at all unless enough liquidity exists to
// finish it completely, checked via OrderBook::liquidityAvailable() before
// a single trade is made.
//
// NOT implemented here: self-trade prevention. OrderNode carries no notion
// of "owner" (see order_book.hpp), so this engine has no way to distinguish
// two orders from the same participant from any other pair. Adding that
// would mean adding an owner field to OrderNode/addOrder first.
//
// CONCURRENCY: like OrderBook, this class does no locking of its own. It's
// meant to be driven by a single caller processing one order at a time, in
// arrival order -- see the note in order_book.hpp about why that
// single-writer discipline is what makes the whole thing safe and
// replayable, rather than anything inside these two classes.
// ============================================================================

#pragma once

#include "order_book.hpp"
#include <algorithm>
#include <vector>

namespace orderbook {

// How an order's price should be interpreted.
enum class OrderType {
    Limit,   // trade only at prices at least as good as `price`; rest the remainder
    Market   // trade at whatever price is available; `price` is ignored
};

// What to do with whatever quantity is left once the match loop stops.
enum class TimeInForce {
    GTC,   // good-til-cancel: rest the remainder in the book (Limit orders only)
    IOC,   // immediate-or-cancel: trade what you can right now, drop the rest
    FOK    // fill-or-kill: trade the ENTIRE order now, or none of it at all
};

struct Order {
    OrderId     id       = 0;
    Side        side     = Side::Buy;
    OrderType   type     = OrderType::Limit;
    TimeInForce tif      = TimeInForce::GTC;
    Price       price    = 0;     // ignored when type == Market
    Quantity    quantity = 0;

    // If true, reject this order instead of matching it -- guarantees it
    // can only ever add liquidity, never take it. Meaningful only for
    // Limit + GTC; combining it with Market is self-defeating (a Market
    // order crosses unconditionally, so it would simply always reject
    // whenever the opposite side is non-empty).
    bool postOnly = false;
};

// One completed trade. Always priced at the RESTING order's price -- see
// crosses()/makeTrade() below for why.
struct Trade {
    OrderId  buyOrderId  = 0;
    OrderId  sellOrderId = 0;
    Price    price       = 0;
    Quantity quantity    = 0;
};

enum class SubmitStatus {
    Accepted,                          // ran (or attempted to run) the match loop
    RejectedPostOnly,                  // would have crossed; postOnly forbids that
    RejectedFokInsufficientLiquidity   // not enough resting quantity to fill it completely
};

struct SubmitResult {
    SubmitStatus       status = SubmitStatus::Accepted;
    std::vector<Trade> trades;
    // Whatever didn't trade: rested in the book (GTC), or discarded
    // (IOC/Market leftover, or a rejection). Check `status` to tell which.
    Quantity remainingQuantity = 0;
};

class MatchingEngine {
public:
    // Non-owning: the caller keeps the OrderBook alive for as long as this
    // engine is in use -- the same relationship a test or a market-data
    // feed would have with it.
    explicit MatchingEngine(OrderBook& book) : book_(book) {}

    SubmitResult submit(const Order& order) {
        if (order.quantity <= 0) {
            throw std::invalid_argument("submit: quantity must be positive");
        }

        const Side restingSide = (order.side == Side::Buy) ? Side::Sell : Side::Buy;
        SubmitResult result;

        if (order.postOnly && wouldCrossImmediately(order, restingSide)) {
            result.status            = SubmitStatus::RejectedPostOnly;
            result.remainingQuantity = order.quantity;
            return result;
        }

        if (order.tif == TimeInForce::FOK) {
            Quantity available = book_.liquidityAvailable(
                restingSide, order.price, order.type == OrderType::Market, order.quantity);
            if (available < order.quantity) {
                result.status            = SubmitStatus::RejectedFokInsufficientLiquidity;
                result.remainingQuantity = order.quantity;
                return result;
            }
        }

        Quantity remaining = order.quantity;
        while (remaining > 0) {
            OrderNode* restingHead = book_.bestLevelHead(restingSide);
            if (restingHead == nullptr || !crosses(order, restingHead->level->price)) {
                break;
            }

            Quantity tradeQty = std::min(remaining, restingHead->quantity);
            result.trades.push_back(makeTrade(order, *restingHead, tradeQty));
            remaining -= tradeQty;

            // Fully consumed -> remove it; otherwise shrink it in place.
            // reduceQuantity() deliberately never re-splices the node
            // within its level's queue, which is what preserves time
            // priority for whatever's left of a partially-filled order.
            if (tradeQty == restingHead->quantity) {
                book_.cancelOrder(restingHead->id);
            } else {
                book_.reduceQuantity(restingHead->id, tradeQty);
            }
        }

        // Only a resting (Limit, GTC) order gets to occupy a spot in the
        // book once the loop stops. Everything else -- IOC/FOK leftover, or
        // an unfilled Market order with nowhere to rest at all -- is simply
        // discarded. (FOK should reach here with remaining == 0 given the
        // precheck above; the branch below is defense-in-depth, not a path
        // this engine expects to actually take.)
        if (remaining > 0 && order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
            book_.addOrder(order.id, order.side, order.price, remaining);
        }

        result.remainingQuantity = remaining;
        return result;
    }

    bool cancel(OrderId id) { return book_.cancelOrder(id); }

private:
    // Whether `order` is still willing to trade against a resting order
    // sitting at `restingPrice`. A Market order crosses unconditionally --
    // it has no price of its own to compare; by definition it takes
    // whatever's best available.
    static bool crosses(const Order& order, Price restingPrice) {
        if (order.type == OrderType::Market) return true;
        return order.side == Side::Buy ? order.price >= restingPrice
                                        : order.price <= restingPrice;
    }

    bool wouldCrossImmediately(const Order& order, Side restingSide) const {
        OrderNode* head = book_.bestLevelHead(restingSide);
        return head != nullptr && crosses(order, head->level->price);
    }

    // Trades always print at the resting order's price, never the incoming
    // order's: the resting side already advertised that price and waited --
    // the incoming side just agreed to accept it.
    static Trade makeTrade(const Order& order, const OrderNode& restingOrder, Quantity qty) {
        Trade trade;
        trade.price    = restingOrder.level->price;
        trade.quantity = qty;
        if (order.side == Side::Buy) {
            trade.buyOrderId  = order.id;
            trade.sellOrderId = restingOrder.id;
        } else {
            trade.buyOrderId  = restingOrder.id;
            trade.sellOrderId = order.id;
        }
        return trade;
    }

    OrderBook& book_;
};

} // namespace orderbook
