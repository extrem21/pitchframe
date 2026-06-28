#include "pitchframe/parser/itch_parser.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace pitchframe;

TEST(SymbolTableTest, InsertAndLookup) {
    SymbolTable st;
    const uint8_t sym8[8] = {'A','A','P','L',' ',' ',' ',' '};
    st.insert(1, sym8);
    ASSERT_NE(st.lookup(1), nullptr);
    EXPECT_STREQ(st.lookup(1), "AAPL");
    EXPECT_EQ(st.count, 1);
}

TEST(SymbolTableTest, TrailingSpacesTrimmed) {
    SymbolTable st;
    const uint8_t sym8[8] = {'M','S','F','T',' ',' ',' ',' '};
    st.insert(10, sym8);
    EXPECT_STREQ(st.lookup(10), "MSFT");
}

TEST(SymbolTableTest, FullEightCharSymbol) {
    SymbolTable st;
    const uint8_t sym8[8] = {'B','R','K','A','L','O','N','G'};
    st.insert(5, sym8);
    EXPECT_STREQ(st.lookup(5), "BRKALONE");
}

TEST(SymbolTableTest, LocateZeroIgnored) {
    SymbolTable st;
    const uint8_t sym8[8] = {'X',' ',' ',' ',' ',' ',' ',' '};
    st.insert(0, sym8);
    EXPECT_EQ(st.count, 0);
}

TEST(SymbolTableTest, LocateAtMaxIgnored) {
    SymbolTable st;
    const uint8_t sym8[8] = {'X',' ',' ',' ',' ',' ',' ',' '};
    st.insert(k_max_locate, sym8);
    EXPECT_EQ(st.count, 0);
}

TEST(SymbolTableTest, LookupMissingReturnsNull) {
    SymbolTable st;
    EXPECT_EQ(st.lookup(42), nullptr);
}

TEST(SymbolTableTest, LookupInvalidLocateReturnsNull) {
    SymbolTable st;
    EXPECT_EQ(st.lookup(0), nullptr);
    EXPECT_EQ(st.lookup(k_max_locate), nullptr);
}
