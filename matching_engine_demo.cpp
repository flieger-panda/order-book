// matching_engine_demo.cpp
//
// Small standalone program exercising MatchingEngine, so the class in
// matching_engine.hpp can be compiled and sanity-checked rather than taken
// on faith. Build with:
//
//   g++ -std=c++17 -Wall -Wextra -O2 matching_engine_demo.cpp -o matching_engine_demo
//   ./matching_engine_demo

#include "matching_engine.hpp"
#include <cassert>
#include <iostream>

using namespace orderbook;

int main() {
    OrderBook book;
    MatchingEngine engine(book);

    // --- Populate the book ---------------------------------------------------
    book.addOrder(/*id=*/401, Side::Buy,  /*price=*/10025, /*qty=*/150);
    book.addOrder(/*id=*/301, Side::Sell, /*price=*/10035, /*qty=*/100);
    book.addOrder(/*id=*/302, Side::Sell, /*price=*/10035, /*qty=*/50);  // same price as 301, arrives after
    book.addOrder(/*id=*/303, Side::Sell, /*price=*/10040, /*qty=*/200);

    // --- GTC limit buy that crosses two price levels, then rests the rest ---
    // 10035 level has 150 total (100 + 50) -> fully consumed.
    // Remaining 150 then eats 150 of the 200 resting at 10040.
    SubmitResult r1 = engine.submit(Order{
        .id = 501, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::GTC,
        .price = 10040, .quantity = 300});
    assert(r1.status == SubmitStatus::Accepted);
    assert(r1.trades.size() == 3);
    assert(r1.trades[0].sellOrderId == 301 && r1.trades[0].price == 10035 && r1.trades[0].quantity == 100);
    assert(r1.trades[1].sellOrderId == 302 && r1.trades[1].price == 10035 && r1.trades[1].quantity == 50);
    assert(r1.trades[2].sellOrderId == 303 && r1.trades[2].price == 10040 && r1.trades[2].quantity == 150);
    assert(r1.remainingQuantity == 0);
    assert(!book.contains(301) && !book.contains(302));
    assert(book.quantityAt(Side::Sell, 10040) == 50);   // 303 shrunk from 200 -> 50
    assert(*book.bestAsk() == 10040);
    std::cout << "[1] GTC limit crossed two levels, rested nothing: "
              << r1.trades.size() << " trades\n";

    // --- IOC that partially fills and drops the remainder --------------------
    book.addOrder(/*id=*/310, Side::Sell, /*price=*/10050, /*qty=*/80);
    SubmitResult r2 = engine.submit(Order{
        .id = 510, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::IOC,
        .price = 10050, .quantity = 200});
    assert(r2.status == SubmitStatus::Accepted);
    assert(r2.trades.size() == 2);          // consumes 303's remaining 50, then all of 310's 80
    assert(r2.remainingQuantity == 70);      // 200 - 50 - 80, dropped rather than rested
    assert(!book.contains(510));
    assert(!book.bestAsk().has_value());     // ask side fully drained
    std::cout << "[2] IOC filled " << (200 - r2.remainingQuantity)
              << ", dropped " << r2.remainingQuantity << " instead of resting it\n";

    // --- FOK rejected: not enough liquidity to fill the whole thing ---------
    book.addOrder(/*id=*/320, Side::Sell, /*price=*/10030, /*qty=*/50);
    SubmitResult r3 = engine.submit(Order{
        .id = 520, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::FOK,
        .price = 10030, .quantity = 100});
    assert(r3.status == SubmitStatus::RejectedFokInsufficientLiquidity);
    assert(r3.trades.empty());
    assert(book.quantityAt(Side::Sell, 10030) == 50);   // untouched -- FOK never touched the book
    assert(!book.contains(520));
    std::cout << "[3] FOK correctly rejected without touching the book\n";

    // --- FOK accepted: exactly enough liquidity available --------------------
    SubmitResult r4 = engine.submit(Order{
        .id = 521, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::FOK,
        .price = 10030, .quantity = 50});
    assert(r4.status == SubmitStatus::Accepted);
    assert(r4.trades.size() == 1 && r4.trades[0].quantity == 50);
    assert(r4.remainingQuantity == 0);
    assert(!book.contains(320));
    std::cout << "[4] FOK filled completely in one trade\n";

    // --- postOnly rejected: order would have crossed immediately -------------
    book.addOrder(/*id=*/330, Side::Sell, /*price=*/10060, /*qty=*/40);
    SubmitResult r5 = engine.submit(Order{
        .id = 530, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::GTC,
        .price = 10060, .quantity = 10, .postOnly = true});
    assert(r5.status == SubmitStatus::RejectedPostOnly);
    assert(r5.trades.empty());
    assert(!book.contains(530));
    assert(book.quantityAt(Side::Sell, 10060) == 40);   // untouched
    std::cout << "[5] postOnly correctly rejected an order that would have crossed\n";

    // --- postOnly accepted: order doesn't cross, rests normally ---------------
    SubmitResult r6 = engine.submit(Order{
        .id = 531, .side = Side::Buy, .type = OrderType::Limit, .tif = TimeInForce::GTC,
        .price = 10020, .quantity = 10, .postOnly = true});
    assert(r6.status == SubmitStatus::Accepted);
    assert(r6.trades.empty());
    assert(book.contains(531));
    assert(book.quantityAt(Side::Buy, 10020) == 10);
    std::cout << "[6] postOnly order that didn't cross rested normally\n";

    // --- Market order drains multiple levels, unfilled remainder is dropped --
    book.addOrder(/*id=*/340, Side::Sell, /*price=*/10061, /*qty=*/20);
    book.addOrder(/*id=*/341, Side::Sell, /*price=*/10065, /*qty=*/30);
    // Ask side now: 330 (10060, 40) + 340 (10061, 20) + 341 (10065, 30) = 90 total.
    SubmitResult r7 = engine.submit(Order{
        .id = 540, .side = Side::Buy, .type = OrderType::Market, .quantity = 200});
    assert(r7.status == SubmitStatus::Accepted);
    assert(r7.trades.size() == 3);
    assert(r7.remainingQuantity == 110);     // 200 - 90, a Market order never rests
    assert(!book.bestAsk().has_value());
    assert(!book.contains(540));
    std::cout << "[7] Market order drained all 90 available and dropped the unfilled 110\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
