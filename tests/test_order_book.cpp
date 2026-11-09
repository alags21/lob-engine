#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

// ---- Event construction helpers (expected values) ----
Event accepted(OrderId id, Side s, Price p, Qty q) {
  Event e;
  e.type = EventType::Accepted;
  e.order_id = id;
  e.side = s;
  e.price = p;
  e.qty = q;
  return e;
}
Event rejected(OrderId id, RejectReason r) {
  Event e;
  e.type = EventType::Rejected;
  e.order_id = id;
  e.reject = r;
  return e;
}
Event trade(OrderId taker, OrderId maker, Side taker_side, Price p, Qty q) {
  Event e;
  e.type = EventType::Trade;
  e.order_id = taker;
  e.maker_id = maker;
  e.side = taker_side;
  e.price = p;
  e.qty = q;
  return e;
}
Event cancelled(OrderId id, Side s, Qty q, CancelReason why, Price p = 0) {
  Event e;
  e.type = EventType::Cancelled;
  e.order_id = id;
  e.side = s;
  e.price = p;
  e.qty = q;
  e.cancel = why;
  return e;
}
Event modified(OrderId id, Side s, Price p, Qty q, bool lost) {
  Event e;
  e.type = EventType::Modified;
  e.order_id = id;
  e.side = s;
  e.price = p;
  e.qty = q;
  e.lost_priority = lost;
  return e;
}

class BookTest : public ::testing::Test {
 protected:
  BookTest() : book([this](const Event& e) { events.push_back(e); }) {}

  RejectReason limit(OrderId id, Side s, Price p, Qty q, TimeInForce tif = TimeInForce::GTC) {
    return book.submit({id, s, OrderType::Limit, tif, p, q});
  }
  RejectReason buy(OrderId id, Price p, Qty q) { return limit(id, Side::Buy, p, q); }
  RejectReason sell(OrderId id, Price p, Qty q) { return limit(id, Side::Sell, p, q); }
  RejectReason market(OrderId id, Side s, Qty q, TimeInForce tif = TimeInForce::GTC) {
    return book.submit({id, s, OrderType::Market, tif, 0, q});
  }
  // Drops events recorded so far so a test can assert only on what follows.
  void clear() { events.clear(); }
  std::vector<Event> take() {
    std::vector<Event> out;
    out.swap(events);
    return out;
  }
  std::vector<Event> trades() const {
    std::vector<Event> out;
    for (const auto& e : events)
      if (e.type == EventType::Trade) out.push_back(e);
    return out;
  }

  OrderBook book;
  std::vector<Event> events;
};

using Events = std::vector<Event>;

}  // namespace

// ---------------------------------------------------------------- empty book

TEST_F(BookTest, EmptyBookHasNoBestPrices) {
  EXPECT_TRUE(book.empty());
  EXPECT_FALSE(book.best_bid());
  EXPECT_FALSE(book.best_ask());
  EXPECT_TRUE(book.levels(Side::Buy).empty());
  EXPECT_TRUE(book.levels(Side::Sell).empty());
  EXPECT_EQ(book.qty_at(Side::Buy, 100), 0);
  EXPECT_TRUE(book.orders_at(Side::Sell, 100).empty());
}

TEST_F(BookTest, MarketOrderOnEmptyBookIsCancelled) {
  EXPECT_EQ(market(1, Side::Buy, 10), RejectReason::None);
  EXPECT_EQ(events, (Events{accepted(1, Side::Buy, 0, 10),
                            cancelled(1, Side::Buy, 10, CancelReason::MarketRemainder)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, IocOnEmptyBookIsCancelled) {
  limit(1, Side::Sell, 100, 5, TimeInForce::IOC);
  EXPECT_EQ(events, (Events{accepted(1, Side::Sell, 100, 5),
                            cancelled(1, Side::Sell, 5, CancelReason::IocRemainder)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, FokOnEmptyBookIsKilled) {
  limit(1, Side::Buy, 100, 5, TimeInForce::FOK);
  EXPECT_EQ(events, (Events{accepted(1, Side::Buy, 100, 5),
                            cancelled(1, Side::Buy, 5, CancelReason::FokUnfillable)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, CancelAndModifyOnEmptyBookAreRejected) {
  EXPECT_EQ(book.cancel(1), RejectReason::UnknownOrder);
  EXPECT_EQ(book.modify(1, 100, 5), RejectReason::UnknownOrder);
  EXPECT_EQ(events, (Events{rejected(1, RejectReason::UnknownOrder),
                            rejected(1, RejectReason::UnknownOrder)}));
}

// ------------------------------------------------------------- limit / resting

TEST_F(BookTest, NonCrossingLimitRests) {
  EXPECT_EQ(buy(1, 99, 10), RejectReason::None);
  EXPECT_EQ(sell(2, 101, 7), RejectReason::None);
  EXPECT_EQ(events, (Events{accepted(1, Side::Buy, 99, 10), accepted(2, Side::Sell, 101, 7)}));
  EXPECT_EQ(book.best_bid(), 99);
  EXPECT_EQ(book.best_ask(), 101);
  EXPECT_EQ(book.qty_at(Side::Buy, 99), 10);
  EXPECT_EQ(book.qty_at(Side::Sell, 101), 7);
  EXPECT_EQ(book.order_count(), 2u);
  auto v = book.find(1);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->side, Side::Buy);
  EXPECT_EQ(v->price, 99);
  EXPECT_EQ(v->qty, 10);
}

TEST_F(BookTest, LevelsAreOrderedBestFirst) {
  buy(1, 98, 1);
  buy(2, 100, 2);
  buy(3, 99, 3);
  sell(4, 103, 1);
  sell(5, 101, 2);
  sell(6, 102, 3);
  EXPECT_EQ(book.levels(Side::Buy),
            (std::vector<LevelInfo>{{100, 2, 1}, {99, 3, 1}, {98, 1, 1}}));
  EXPECT_EQ(book.levels(Side::Sell),
            (std::vector<LevelInfo>{{101, 2, 1}, {102, 3, 1}, {103, 1, 1}}));
}

TEST_F(BookTest, TouchingPricesCross) {
  sell(1, 100, 5);
  clear();
  buy(2, 100, 5);  // equal price is marketable
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 100, 5), trade(2, 1, Side::Buy, 100, 5)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, ExecutesAtMakerPriceNotTakerPrice) {
  sell(1, 100, 5);
  clear();
  buy(2, 105, 5);  // willing to pay 105, trades at maker's 100
  EXPECT_EQ(trades(), (Events{trade(2, 1, Side::Buy, 100, 5)}));

  buy(3, 95, 5);
  clear();
  sell(4, 90, 5);  // willing to sell at 90, trades at maker's 95
  EXPECT_EQ(trades(), (Events{trade(4, 3, Side::Sell, 95, 5)}));
}

TEST_F(BookTest, PartialFillOfTakerRestsRemainder) {
  sell(1, 100, 4);
  clear();
  buy(2, 100, 10);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 100, 10), trade(2, 1, Side::Buy, 100, 4)}));
  EXPECT_FALSE(book.contains(1));
  auto v = book.find(2);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->qty, 6);
  EXPECT_EQ(book.best_bid(), 100);
  EXPECT_FALSE(book.best_ask());
}

TEST_F(BookTest, PartialFillOfMakerKeepsRemainderAtFront) {
  sell(1, 100, 10);
  sell(2, 100, 10);
  clear();
  buy(3, 100, 3);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Buy, 100, 3)}));
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{1, 7}, {2, 10}}));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 17);
}

TEST_F(BookTest, PartialFillAcrossMultipleLevels) {
  sell(1, 100, 5);
  sell(2, 101, 5);
  sell(3, 102, 5);
  clear();
  buy(4, 102, 12);
  EXPECT_EQ(events, (Events{accepted(4, Side::Buy, 102, 12), trade(4, 1, Side::Buy, 100, 5),
                            trade(4, 2, Side::Buy, 101, 5), trade(4, 3, Side::Buy, 102, 2)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 102), 3);
  EXPECT_EQ(book.best_ask(), 102);
  EXPECT_FALSE(book.best_bid());
}

TEST_F(BookTest, SellSweepsBidsHighestFirst) {
  buy(1, 98, 5);
  buy(2, 100, 5);
  buy(3, 99, 5);
  clear();
  sell(4, 98, 12);
  EXPECT_EQ(trades(), (Events{trade(4, 2, Side::Sell, 100, 5), trade(4, 3, Side::Sell, 99, 5),
                              trade(4, 1, Side::Sell, 98, 2)}));
}

TEST_F(BookTest, LimitStopsAtItsLimitAndRestsOnOtherSide) {
  sell(1, 100, 5);
  sell(2, 102, 5);
  clear();
  buy(3, 101, 8);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Buy, 100, 5)}));
  EXPECT_EQ(book.best_bid(), 101);
  EXPECT_EQ(book.qty_at(Side::Buy, 101), 3);
  EXPECT_EQ(book.best_ask(), 102);
}

TEST_F(BookTest, ExhaustedLevelIsRemoved) {
  sell(1, 100, 5);
  buy(2, 100, 5);
  EXPECT_TRUE(book.levels(Side::Sell).empty());
  EXPECT_FALSE(book.best_ask());
}

// -------------------------------------------------------------- FIFO / priority

TEST_F(BookTest, FifoWithinLevel) {
  sell(1, 100, 5);
  sell(2, 100, 5);
  sell(3, 100, 5);
  clear();
  buy(4, 100, 12);
  EXPECT_EQ(trades(), (Events{trade(4, 1, Side::Buy, 100, 5), trade(4, 2, Side::Buy, 100, 5),
                              trade(4, 3, Side::Buy, 100, 2)}));
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{3, 3}}));
}

TEST_F(BookTest, BetterPriceBeatsEarlierTime) {
  sell(1, 101, 5);  // earlier but worse price
  sell(2, 100, 5);
  clear();
  buy(3, 101, 5);
  EXPECT_EQ(trades(), (Events{trade(3, 2, Side::Buy, 100, 5)}));
}

TEST_F(BookTest, FifoOnBidSide) {
  buy(1, 100, 5);
  buy(2, 100, 5);
  clear();
  sell(3, 100, 6);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Sell, 100, 5), trade(3, 2, Side::Sell, 100, 1)}));
}

TEST_F(BookTest, QueueOrderPreservedAfterMiddleCancel) {
  sell(1, 100, 1);
  sell(2, 100, 1);
  sell(3, 100, 1);
  book.cancel(2);
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{1, 1}, {3, 1}}));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 2);
}

// ---------------------------------------------------------------- self-crossing

TEST_F(BookTest, OrderCrossingItsOwnRestingSideTrades) {
  // No participant identity => no self-trade prevention; documented behaviour.
  buy(1, 100, 5);
  clear();
  sell(2, 99, 5);
  EXPECT_EQ(trades(), (Events{trade(2, 1, Side::Sell, 100, 5)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, BookNeverRestsCrossed) {
  buy(1, 100, 5);
  sell(2, 101, 5);
  buy(3, 101, 2);   // trades
  sell(4, 100, 10); // trades, rests 8 @100 minus fills
  ASSERT_TRUE(book.best_bid() || book.best_ask());
  if (book.best_bid() && book.best_ask()) {
    EXPECT_LT(*book.best_bid(), *book.best_ask());
  }
}

// ---------------------------------------------------------------------- market

TEST_F(BookTest, MarketOrderSweepsLevels) {
  sell(1, 100, 5);
  sell(2, 105, 5);
  clear();
  market(3, Side::Buy, 8);
  EXPECT_EQ(events, (Events{accepted(3, Side::Buy, 0, 8), trade(3, 1, Side::Buy, 100, 5),
                            trade(3, 2, Side::Buy, 105, 3)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 105), 2);
}

TEST_F(BookTest, MarketOrderRemainderIsCancelledNotRested) {
  buy(1, 100, 4);
  clear();
  market(2, Side::Sell, 10);
  EXPECT_EQ(events, (Events{accepted(2, Side::Sell, 0, 10), trade(2, 1, Side::Sell, 100, 4),
                            cancelled(2, Side::Sell, 6, CancelReason::MarketRemainder)}));
  EXPECT_TRUE(book.empty());
  EXPECT_FALSE(book.contains(2));
}

TEST_F(BookTest, MarketOrderIgnoresPriceField) {
  sell(1, 500, 5);
  clear();
  book.submit({2, Side::Buy, OrderType::Market, TimeInForce::GTC, 1, 5});  // price 1 ignored
  EXPECT_EQ(trades(), (Events{trade(2, 1, Side::Buy, 500, 5)}));
}

TEST_F(BookTest, MarketIocBehavesLikeMarket) {
  sell(1, 100, 3);
  clear();
  market(2, Side::Buy, 5, TimeInForce::IOC);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 0, 5), trade(2, 1, Side::Buy, 100, 3),
                            cancelled(2, Side::Buy, 2, CancelReason::MarketRemainder)}));
}

TEST_F(BookTest, MarketFokKilledWhenInsufficientLiquidity) {
  sell(1, 100, 3);
  clear();
  market(2, Side::Buy, 5, TimeInForce::FOK);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 0, 5),
                            cancelled(2, Side::Buy, 5, CancelReason::FokUnfillable)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 3);  // untouched
}

TEST_F(BookTest, MarketFokFilledWhenLiquiditySpansLevels) {
  sell(1, 100, 3);
  sell(2, 200, 3);
  clear();
  market(3, Side::Buy, 5, TimeInForce::FOK);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Buy, 100, 3), trade(3, 2, Side::Buy, 200, 2)}));
}

// ------------------------------------------------------------------------- IOC

TEST_F(BookTest, IocFullFillLeavesNoRemainder) {
  sell(1, 100, 5);
  clear();
  limit(2, Side::Buy, 100, 5, TimeInForce::IOC);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 100, 5), trade(2, 1, Side::Buy, 100, 5)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, IocPartialFillCancelsRemainder) {
  sell(1, 100, 3);
  clear();
  limit(2, Side::Buy, 100, 5, TimeInForce::IOC);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 100, 5), trade(2, 1, Side::Buy, 100, 3),
                            cancelled(2, Side::Buy, 2, CancelReason::IocRemainder)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, IocNonMarketablePriceDoesNotRest) {
  sell(1, 105, 3);
  clear();
  limit(2, Side::Buy, 100, 5, TimeInForce::IOC);
  EXPECT_EQ(events, (Events{accepted(2, Side::Buy, 100, 5),
                            cancelled(2, Side::Buy, 5, CancelReason::IocRemainder)}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST_F(BookTest, IocRespectsLimitPrice) {
  sell(1, 100, 2);
  sell(2, 102, 2);
  clear();
  limit(3, Side::Buy, 101, 10, TimeInForce::IOC);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Buy, 100, 2)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 102), 2);
}

// ------------------------------------------------------------------------- FOK

TEST_F(BookTest, FokFullyFilledAcrossLevels) {
  sell(1, 100, 3);
  sell(2, 101, 3);
  clear();
  limit(3, Side::Buy, 101, 5, TimeInForce::FOK);
  EXPECT_EQ(events, (Events{accepted(3, Side::Buy, 101, 5), trade(3, 1, Side::Buy, 100, 3),
                            trade(3, 2, Side::Buy, 101, 2)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 101), 1);
}

TEST_F(BookTest, FokExactlyEqualLiquiditySucceeds) {
  sell(1, 100, 5);
  clear();
  limit(2, Side::Buy, 100, 5, TimeInForce::FOK);
  EXPECT_EQ(trades(), (Events{trade(2, 1, Side::Buy, 100, 5)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, FokKilledWithNoPartialTrades) {
  sell(1, 100, 3);
  sell(2, 101, 3);
  clear();
  limit(3, Side::Buy, 101, 7, TimeInForce::FOK);
  EXPECT_EQ(events, (Events{accepted(3, Side::Buy, 101, 7),
                            cancelled(3, Side::Buy, 7, CancelReason::FokUnfillable)}));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 3);
  EXPECT_EQ(book.qty_at(Side::Sell, 101), 3);
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{1, 3}}));
}

TEST_F(BookTest, FokDoesNotCountLiquidityBeyondItsLimit) {
  sell(1, 100, 3);
  sell(2, 110, 100);
  clear();
  limit(3, Side::Buy, 105, 5, TimeInForce::FOK);
  EXPECT_EQ(events, (Events{accepted(3, Side::Buy, 105, 5),
                            cancelled(3, Side::Buy, 5, CancelReason::FokUnfillable)}));
}

TEST_F(BookTest, FokSellSide) {
  buy(1, 100, 4);
  buy(2, 99, 4);
  clear();
  limit(3, Side::Sell, 99, 8, TimeInForce::FOK);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Sell, 100, 4), trade(3, 2, Side::Sell, 99, 4)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, FokNeverRests) {
  limit(1, Side::Buy, 100, 5, TimeInForce::FOK);
  EXPECT_FALSE(book.contains(1));
}

// ---------------------------------------------------------------------- cancel

TEST_F(BookTest, CancelRemovesOrderAndEmitsRemainingQty) {
  buy(1, 100, 10);
  sell(2, 105, 5);
  clear();
  EXPECT_EQ(book.cancel(1), RejectReason::None);
  EXPECT_EQ(events, (Events{cancelled(1, Side::Buy, 10, CancelReason::UserRequest, 100)}));
  EXPECT_FALSE(book.contains(1));
  EXPECT_FALSE(book.best_bid());
  EXPECT_EQ(book.best_ask(), 105);
}

TEST_F(BookTest, CancelPartiallyFilledReportsRemainder) {
  sell(1, 100, 10);
  buy(2, 100, 4);
  clear();
  book.cancel(1);
  EXPECT_EQ(events, (Events{cancelled(1, Side::Sell, 6, CancelReason::UserRequest, 100)}));
}

TEST_F(BookTest, CancelOfFilledOrderIsRejected) {
  sell(1, 100, 5);
  buy(2, 100, 5);
  clear();
  EXPECT_EQ(book.cancel(1), RejectReason::UnknownOrder);
  EXPECT_EQ(book.cancel(2), RejectReason::UnknownOrder);
  EXPECT_EQ(events, (Events{rejected(1, RejectReason::UnknownOrder),
                            rejected(2, RejectReason::UnknownOrder)}));
}

TEST_F(BookTest, DoubleCancelSecondIsRejected) {
  buy(1, 100, 5);
  EXPECT_EQ(book.cancel(1), RejectReason::None);
  EXPECT_EQ(book.cancel(1), RejectReason::UnknownOrder);
}

TEST_F(BookTest, CancelOneOfManyAtLevelKeepsLevel) {
  buy(1, 100, 5);
  buy(2, 100, 7);
  book.cancel(1);
  EXPECT_EQ(book.levels(Side::Buy), (std::vector<LevelInfo>{{100, 7, 1}}));
}

TEST_F(BookTest, CancelLastOrderRemovesLevel) {
  buy(1, 100, 5);
  buy(2, 99, 5);
  book.cancel(1);
  EXPECT_EQ(book.best_bid(), 99);
  EXPECT_EQ(book.levels(Side::Buy).size(), 1u);
}

TEST_F(BookTest, CancelledOrderIsNotMatched) {
  sell(1, 100, 5);
  sell(2, 100, 5);
  book.cancel(1);
  clear();
  buy(3, 100, 5);
  EXPECT_EQ(trades(), (Events{trade(3, 2, Side::Buy, 100, 5)}));
}

// ---------------------------------------------------------------------- modify

TEST_F(BookTest, ModifyReduceSizeKeepsPriority) {
  sell(1, 100, 10);
  sell(2, 100, 10);
  clear();
  EXPECT_EQ(book.modify(1, 100, 4), RejectReason::None);
  EXPECT_EQ(events, (Events{modified(1, Side::Sell, 100, 4, false)}));
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{1, 4}, {2, 10}}));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 14);
  clear();
  buy(3, 100, 5);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Buy, 100, 4),
                              trade(3, 2, Side::Buy, 100, 1)}));
}

TEST_F(BookTest, ModifyIncreaseSizeLosesPriority) {
  buy(1, 100, 5);
  buy(2, 100, 5);
  clear();
  book.modify(1, 100, 8);
  EXPECT_EQ(events, (Events{modified(1, Side::Buy, 100, 8, true)}));
  EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<RestingOrder>{{2, 5}, {1, 8}}));
  EXPECT_EQ(book.qty_at(Side::Buy, 100), 13);
}

TEST_F(BookTest, ModifyPriceMovesOrderToBackOfNewLevel) {
  buy(1, 100, 5);
  buy(2, 101, 5);
  book.modify(1, 101, 5);
  EXPECT_EQ(book.orders_at(Side::Buy, 101), (std::vector<RestingOrder>{{2, 5}, {1, 5}}));
  EXPECT_EQ(book.qty_at(Side::Buy, 100), 0);
  EXPECT_EQ(book.levels(Side::Buy).size(), 1u);
}

TEST_F(BookTest, ModifySameValuesIsNoOpKeepingPriority) {
  buy(1, 100, 5);
  buy(2, 100, 5);
  clear();
  EXPECT_EQ(book.modify(1, 100, 5), RejectReason::None);
  EXPECT_EQ(events, (Events{modified(1, Side::Buy, 100, 5, false)}));
  EXPECT_EQ(book.orders_at(Side::Buy, 100), (std::vector<RestingOrder>{{1, 5}, {2, 5}}));
}

TEST_F(BookTest, ModifyIntoCrossingPriceTradesAsTaker) {
  sell(1, 100, 4);
  buy(2, 95, 10);
  clear();
  book.modify(2, 100, 10);
  EXPECT_EQ(events, (Events{modified(2, Side::Buy, 100, 10, true),
                            trade(2, 1, Side::Buy, 100, 4)}));
  EXPECT_FALSE(book.contains(1));
  EXPECT_EQ(book.find(2)->qty, 6);
  EXPECT_EQ(book.find(2)->price, 100);
  EXPECT_EQ(book.best_bid(), 100);
}

TEST_F(BookTest, ModifyCrossingFullyFillsAndLeavesNothing) {
  sell(1, 100, 10);
  buy(2, 95, 5);
  book.modify(2, 100, 5);
  EXPECT_FALSE(book.contains(2));
  EXPECT_EQ(book.qty_at(Side::Sell, 100), 5);
}

TEST_F(BookTest, ModifyDoesNotMatchAgainstItself) {
  buy(1, 100, 5);
  sell(2, 110, 5);
  clear();
  book.modify(1, 109, 5);  // stays below the ask; no trade
  EXPECT_TRUE(trades().empty());
  EXPECT_EQ(book.best_bid(), 109);
}

TEST_F(BookTest, ModifyReducedOrderKeepsPartialFillSemantics) {
  sell(1, 100, 10);
  buy(2, 100, 4);  // order 1 has 6 left
  book.modify(1, 100, 2);
  EXPECT_EQ(book.find(1)->qty, 2);
  // new_qty greater than remaining is an increase => loses priority.
  sell(3, 100, 1);
  clear();
  book.modify(1, 100, 9);
  EXPECT_EQ(book.orders_at(Side::Sell, 100), (std::vector<RestingOrder>{{3, 1}, {1, 9}}));
}

TEST_F(BookTest, ModifyRejections) {
  buy(1, 100, 5);
  clear();
  EXPECT_EQ(book.modify(1, 100, 0), RejectReason::InvalidQuantity);
  EXPECT_EQ(book.modify(1, 100, -3), RejectReason::InvalidQuantity);
  EXPECT_EQ(book.modify(1, 0, 5), RejectReason::InvalidPrice);
  EXPECT_EQ(book.modify(9, 100, 5), RejectReason::UnknownOrder);
  EXPECT_EQ(events, (Events{rejected(1, RejectReason::InvalidQuantity),
                            rejected(1, RejectReason::InvalidQuantity),
                            rejected(1, RejectReason::InvalidPrice),
                            rejected(9, RejectReason::UnknownOrder)}));
  EXPECT_EQ(book.find(1)->qty, 5);  // unchanged
}

TEST_F(BookTest, ModifyOfFilledOrderIsRejected) {
  sell(1, 100, 5);
  buy(2, 100, 5);
  EXPECT_EQ(book.modify(1, 100, 3), RejectReason::UnknownOrder);
}

TEST_F(BookTest, ModifyReleasedLevelIsRemoved) {
  buy(1, 100, 5);
  book.modify(1, 90, 5);
  EXPECT_EQ(book.levels(Side::Buy), (std::vector<LevelInfo>{{90, 5, 1}}));
}

// ----------------------------------------------------------------- validation

TEST_F(BookTest, RejectsNonPositiveQuantity) {
  EXPECT_EQ(buy(1, 100, 0), RejectReason::InvalidQuantity);
  EXPECT_EQ(buy(2, 100, -5), RejectReason::InvalidQuantity);
  EXPECT_EQ(market(3, Side::Buy, 0), RejectReason::InvalidQuantity);
  EXPECT_EQ(events, (Events{rejected(1, RejectReason::InvalidQuantity),
                            rejected(2, RejectReason::InvalidQuantity),
                            rejected(3, RejectReason::InvalidQuantity)}));
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, RejectsNonPositiveLimitPrice) {
  EXPECT_EQ(buy(1, 0, 5), RejectReason::InvalidPrice);
  EXPECT_EQ(sell(2, -1, 5), RejectReason::InvalidPrice);
  EXPECT_TRUE(book.empty());
}

TEST_F(BookTest, RejectsDuplicateLiveId) {
  buy(1, 100, 5);
  clear();
  EXPECT_EQ(sell(1, 200, 5), RejectReason::DuplicateId);
  EXPECT_EQ(events, (Events{rejected(1, RejectReason::DuplicateId)}));
  EXPECT_EQ(book.order_count(), 1u);
  EXPECT_EQ(book.find(1)->side, Side::Buy);
}

TEST_F(BookTest, IdCanBeReusedAfterFill) {
  sell(1, 100, 5);
  buy(2, 100, 5);
  EXPECT_EQ(buy(1, 50, 1), RejectReason::None);  // 1 is no longer live
  EXPECT_TRUE(book.contains(1));
}

TEST_F(BookTest, WorksWithoutEventHandler) {
  OrderBook silent;
  EXPECT_EQ(silent.submit({1, Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 5}),
            RejectReason::None);
  EXPECT_EQ(silent.cancel(1), RejectReason::None);
  EXPECT_EQ(silent.cancel(1), RejectReason::UnknownOrder);
}

TEST_F(BookTest, LargeQuantitiesAndPricesDoNotOverflowAggregates) {
  const Qty big = Qty{1} << 40;
  buy(1, 1'000'000'000, big);
  buy(2, 1'000'000'000, big);
  EXPECT_EQ(book.qty_at(Side::Buy, 1'000'000'000), big * 2);
  clear();
  sell(3, 1'000'000'000, big + 1);
  EXPECT_EQ(trades(), (Events{trade(3, 1, Side::Sell, 1'000'000'000, big),
                              trade(3, 2, Side::Sell, 1'000'000'000, 1)}));
}

// --------------------------------------------------------- event ordering/sums

TEST_F(BookTest, TradeQuantitiesSumToFilledQuantity) {
  for (OrderId i = 1; i <= 5; ++i) sell(i, 100 + static_cast<Price>(i), 3);
  clear();
  buy(10, 110, 100);
  Qty sum = 0;
  for (const auto& t : trades()) sum += t.qty;
  EXPECT_EQ(sum, 15);
  EXPECT_EQ(book.find(10)->qty, 85);
}

TEST_F(BookTest, AcceptedAlwaysPrecedesTrades) {
  sell(1, 100, 5);
  clear();
  buy(2, 100, 5);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].type, EventType::Accepted);
  EXPECT_EQ(events[1].type, EventType::Trade);
}
