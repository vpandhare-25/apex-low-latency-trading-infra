#include "apex/book_builder.hpp"

namespace apex {

void BookBuilder::on_stock_directory(const itch::StockDirectory& m) {
    if (m.symbol() == symbol_) locate_ = m.locate;
}

void BookBuilder::on_add(const itch::AddOrder& m) {
    if (!is_ours(m.locate)) return;
    ++stats_.adds;
    after_update(book_.add(m.id, m.side, m.price, m.shares));
}

void BookBuilder::on_execute(const itch::OrderExecuted& m) {
    if (!is_ours(m.locate)) return;
    ++stats_.executes;
    // For 'C' messages the trade price may differ from the order's price, but the
    // order still loses the same number of shares, so the book update is identical.
    after_update(book_.execute(m.id, m.shares));
}

void BookBuilder::on_cancel(const itch::OrderCancel& m) {
    if (!is_ours(m.locate)) return;
    ++stats_.cancels;
    after_update(book_.cancel(m.id, m.shares));
}

void BookBuilder::on_delete(const itch::OrderDelete& m) {
    if (!is_ours(m.locate)) return;
    ++stats_.deletes;
    after_update(book_.remove(m.id));
}

void BookBuilder::on_replace(const itch::OrderReplace& m) {
    if (!is_ours(m.locate)) return;
    ++stats_.replaces;
    after_update(book_.replace(m.old_id, m.new_id, m.price, m.shares));
}

void BookBuilder::after_update(bool ok) {
    if (!ok) ++stats_.rejected;
    const auto bid = book_.best_bid();
    const auto ask = book_.best_ask();
    if (bid && ask && bid->price >= ask->price) ++stats_.crossed_updates;
}

}  // namespace apex
