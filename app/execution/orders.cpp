// This file was generated with the help of Claude Code.

#include "app/execution/orders.h"

namespace app {

void OrderManager::SendQuote(sim::Side side, sim::Price price, std::uint32_t shares) {
    Quote& q = quote(side);
    const sim::Token token = next_token_++;
    const sim::Timestamp now = session_.Now();

    history_.push_back({now, side, price, shares, token});
    log_.Record(now, q.live ? "REPLACE" : "ENTER", side, price, shares, token);

    if (q.live) {
        sim::oe::ReplaceOrder msg{};
        msg.existing_token = q.token;
        msg.replacement_token = token;
        msg.shares = shares;
        msg.price = price;
        session_.Send(msg);
    } else {
        sim::oe::EnterOrder msg{};
        msg.token = token;
        msg.side = side;
        msg.shares = shares;
        msg.instrument = sim::kInstrument;
        msg.price = price;
        msg.time_in_force = sim::oe::TimeInForce::Day;
        session_.Send(msg);
    }
    q.pending = true;
    q.pending_token = token;
}

void OrderManager::CancelToken(sim::Token token) {
    sim::oe::CancelOrder msg{};
    msg.token = token;
    msg.shares = 0;
    session_.Send(msg);
}

void OrderManager::CancelAll() {
    for (Quote* q : {&bid_, &ask_}) {
        // A pending Replace kills the resting order when it arrives, so its
        // replacement token has to be canceled too; the exchange ignores
        // cancels for tokens that are already dead.
        if (q->live) {
            CancelToken(q->token);
        }
        if (q->pending) {
            CancelToken(q->pending_token);
        }
    }
}

Quote* OrderManager::FindByToken(sim::Token token) {
    for (Quote* q : {&bid_, &ask_}) {
        if ((q->live && q->token == token) || (q->pending && q->pending_token == token)) {
            return q;
        }
    }
    return nullptr;
}

void OrderManager::OnAccepted(const sim::oe::Accepted& msg) {
    Quote& q = quote(msg.side);
    if (!q.pending || q.pending_token != msg.token) {
        return;
    }
    q.live = true;
    q.token = msg.token;
    q.price = msg.price;
    q.open_shares = msg.shares;
    q.pending = false;
}

void OrderManager::OnReplaced(const sim::oe::Replaced& msg) {
    Quote& q = quote(msg.side);
    if (!q.pending || q.pending_token != msg.replacement_token) {
        return;
    }
    q.live = true;
    q.token = msg.replacement_token;
    q.price = msg.price;
    q.open_shares = msg.shares;
    q.pending = false;
}

void OrderManager::OnExecuted(const sim::oe::Executed& msg) {
    Quote* q = FindByToken(msg.token);
    if (q == nullptr) {
        return;
    }
    const bool buy = q == &bid_;
    const auto shares = static_cast<std::int64_t>(msg.executed_shares);
    position_ += buy ? shares : -shares;
    cash_ += buy ? -shares * msg.price : shares * msg.price;

    if (q->live && q->token == msg.token) {
        q->open_shares -= msg.executed_shares;
        if (q->open_shares == 0) {
            q->live = false;
        }
    }
}

void OrderManager::OnRejected(const sim::oe::Rejected& msg) {
    // A rejected Replace means the resting order was already gone (filled
    // while the Replace was in flight); its fill has already been processed.
    for (Quote* q : {&bid_, &ask_}) {
        if (q->pending && q->pending_token == msg.token) {
            q->pending = false;
        }
    }
}

void OrderManager::OnCanceled(const sim::oe::Canceled& msg) {
    for (Quote* q : {&bid_, &ask_}) {
        if (q->live && q->token == msg.token) {
            q->live = false;
        }
        if (q->pending && q->pending_token == msg.token) {
            q->pending = false;
        }
    }
}

}  // namespace app
