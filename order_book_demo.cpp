// order_book_demo.cpp
//
// Small standalone program exercising OrderBook, so the class in
// order_book.hpp can be compiled and sanity-checked rather than taken on
// faith. Build with:
//
//   g++ -std=c++17 -Wall -Wextra -O2 order_book_demo.cpp -o order_book_demo
//   ./order_book_demo

#include "order_book.hpp"
#include <cassert>
#include <iostream>

using namespace orderbook;

int main() {
    OrderBook book;

    // --- Populate both sides ------------------------------------------------
    book.addOrder(/*id=*/104, Side::Buy,  /*price=*/10025, /*qty=*/340);
    book.addOrder(/*id=*/107, Side::Buy,  /*price=*/10025, /*qty=*/120); // same price as 104, arrives after
    book.addOrder(/*id=*/110, Side::Buy,  /*price=*/10020, /*qty=*/210);
    book.addOrder(/*id=*/203, Side::Sell, /*price=*/10035, /*qty=*/290);
    book.addOrder(/*id=*/205, Side::Sell, /*price=*/10035, /*qty=*/460);
    book.addOrder(/*id=*/210, Side::Sell, /*price=*/10040, /*qty=*/200);

    std::cout << "orders in book: " << book.orderCount() << "\n";
    assert(book.orderCount() == 6);

    // --- Best price / depth queries -----------------------------------------
    assert(book.bestBid().has_value() && *book.bestBid() == 10025);
    assert(book.bestAsk().has_value() && *book.bestAsk() == 10035);
    std::cout << "best bid: " << *book.bestBid()
              << "  best ask: " << *book.bestAsk() << "\n";

    // 104 (340) + 107 (120) both rest at 10025 -> aggregate should be 460.
    assert(book.quantityAt(Side::Buy, 10025) == 460);
    std::cout << "quantity at best bid: " << book.quantityAt(Side::Buy, 10025) << "\n";

    // --- Price-time priority check ------------------------------------------
    // 104 arrived before 107 at the same price, so 104 must be the head.
    OrderNode* headAtBest = book.bestLevelHead(Side::Buy);
    assert(headAtBest != nullptr && headAtBest->id == 104);
    std::cout << "head of queue at best bid: order " << headAtBest->id << "\n";

    // --- Partial fill via reduceQuantity -------------------------------------
    book.reduceQuantity(104, 100);                 // 340 -> 240
    assert(book.quantityAt(Side::Buy, 10025) == 360); // 240 + 120
    std::cout << "quantity at best bid after partial fill: "
              << book.quantityAt(Side::Buy, 10025) << "\n";

    // --- Cancel: middle-of-book order (110 is alone at 10020) ---------------
    bool cancelled = book.cancelOrder(110);
    assert(cancelled);
    assert(!book.contains(110));
    assert(book.quantityAt(Side::Buy, 10020) == 0); // level should be gone entirely now
    std::cout << "order 110 cancelled; level 10020 emptied\n";

    // --- Cancel: fully consuming the last order at a price removes the level ---
    book.cancelOrder(104);
    book.cancelOrder(107);
    assert(!book.bestBid().has_value() || *book.bestBid() != 10025);
    std::cout << "best bid after clearing 10025: "
              << (book.bestBid() ? std::to_string(*book.bestBid()) : "none")
              << "\n";

    // --- Cancelling something that isn't there is a no-op, not an error -----
    assert(book.cancelOrder(9999) == false);
    std::cout << "cancel of unknown id correctly returned false\n";

    // --- Duplicate id insertion is rejected -----------------------------------
    bool threw = false;
    try {
        book.addOrder(203, Side::Sell, 10050, 50); // 203 already exists
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "duplicate order id correctly rejected\n";

    std::cout << "\nall checks passed\n";
    return 0;
}