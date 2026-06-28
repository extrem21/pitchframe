#include "pitchframe/book/order_book.h"
#include <gtest/gtest.h>

using namespace pitchframe;

// $50.00 in ITCH fixed-point (4 implied decimal places)
static constexpr uint32_t k_base = 500000;
static constexpr uint16_t k_loc  = 1;

TEST(OrderBookTest, EmptyBookReturnsZero) {
    OrderBook book;
    EXPECT_EQ(book.best_bid(), 0u);
    EXPECT_EQ(book.best_ask(), 0u);
    EXPECT_EQ(book.active_orders(), 0);
}

TEST(OrderBookTest, AddBidSetsBestBid) {
    OrderBook book;
    EXPECT_TRUE(book.add(1, 'B', k_base, 100, k_loc));
    EXPECT_EQ(book.best_bid(), k_base);
    EXPECT_EQ(book.active_orders(), 1);
}

TEST(OrderBookTest, AddAskSetsBestAsk) {
    OrderBook book;
    EXPECT_TRUE(book.add(1, 'S', k_base, 200, k_loc));
    EXPECT_EQ(book.best_ask(), k_base);
}

TEST(OrderBookTest, BestBidIsHighestPrice) {
    OrderBook book;
    book.add(1, 'B', k_base,     100, k_loc);
    book.add(2, 'B', k_base + 1, 100, k_loc);
    EXPECT_EQ(book.best_bid(), k_base + 1);
}

TEST(OrderBookTest, BestAskIsLowestPrice) {
    OrderBook book;
    book.add(1, 'S', k_base + 2, 100, k_loc);
    book.add(2, 'S', k_base + 1, 100, k_loc);
    EXPECT_EQ(book.best_ask(), k_base + 1);
}

TEST(OrderBookTest, CancelReducesSharesButKeepsOrder) {
    OrderBook book;
    book.add(1, 'B', k_base, 100, k_loc);
    EXPECT_TRUE(book.cancel(1, 40));
    // Level still non-empty (60 shares remain); best_bid unchanged.
    EXPECT_EQ(book.best_bid(), k_base);
    EXPECT_EQ(book.active_orders(), 1);
}

TEST(OrderBookTest, CancelUnknownRefReturnsFalse) {
    OrderBook book;
    EXPECT_FALSE(book.cancel(99, 10));
}

TEST(OrderBookTest, RemoveClearsLevel) {
    OrderBook book;
    book.add(1, 'B', k_base, 100, k_loc);
    EXPECT_TRUE(book.remove(1));
    EXPECT_EQ(book.best_bid(), 0u);
    EXPECT_EQ(book.active_orders(), 0);
}

TEST(OrderBookTest, RemoveUnknownRefReturnsFalse) {
    OrderBook book;
    EXPECT_FALSE(book.remove(99));
}

TEST(OrderBookTest, RemoveOneOfTwoOrdersAtSamePrice) {
    OrderBook book;
    book.add(1, 'B', k_base, 100, k_loc);
    book.add(2, 'B', k_base, 200, k_loc);
    book.remove(1);
    EXPECT_EQ(book.best_bid(), k_base);  // level still has order 2
    EXPECT_EQ(book.active_orders(), 1);
    book.remove(2);
    EXPECT_EQ(book.best_bid(), 0u);      // now empty
}

TEST(OrderBookTest, ReplaceMovesToNewPrice) {
    OrderBook book;
    book.add(1, 'B', k_base, 100, k_loc);
    EXPECT_TRUE(book.replace(1, 2, 50, k_base + 5));
    EXPECT_EQ(book.best_bid(), k_base + 5);
    EXPECT_EQ(book.active_orders(), 1);
    // Old price level should be empty.
    book.remove(2);
    EXPECT_EQ(book.best_bid(), 0u);
}

TEST(OrderBookTest, ReplaceUnknownRefReturnsFalse) {
    OrderBook book;
    EXPECT_FALSE(book.replace(99, 100, 50, k_base));
}

TEST(OrderBookTest, OutOfBandPriceUsesOverflow) {
    OrderBook book;
    book.add(1, 'B', k_base, 100, k_loc);  // sets base_price_ = k_base
    uint32_t far = k_base + OrderBook::k_half_band + 100;
    book.add(2, 'S', far, 50, k_loc);
    EXPECT_EQ(book.best_ask(), far);
    book.remove(2);
    EXPECT_EQ(book.best_ask(), 0u);
}

TEST(OrderBookTest, BidAndAskBothTracked) {
    OrderBook book;
    book.add(1, 'B', k_base - 1, 100, k_loc);
    book.add(2, 'S', k_base + 1, 100, k_loc);
    EXPECT_EQ(book.best_bid(), k_base - 1);
    EXPECT_EQ(book.best_ask(), k_base + 1);
}
