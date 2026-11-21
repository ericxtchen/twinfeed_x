#include "trial.h"
#include "../../apps/replay_service.h"

#include <poll.h>
#include <limits>
#include <thread>

namespace {
using namespace bench;

// Sleeping the full inter-event interval tends to turn a tight stream into bursts.
// Sleep through long gaps, then spend the last 50 us pacing on the publisher's CPU.
void wait_until_due(Time due) {
    if (Clock::now() + 50us < due) std::this_thread::sleep_until(due - 50us);
    while (Clock::now() < due) {}
}

struct Sends {
    Count a = 0;
    Count b = 0;
    Count errors = 0;
    Count in_window = 0;
};

// This matches the demo publisher: serve retained frames after the live send loop.
// A replay makes the trial visible as a recovery run in the summary.
Count serve_drain(const loopback::Socket& replay, const publisher::Feed& feed, Time stop) {
    pollfd fd{replay.fd(), POLLIN, 0};
    Count sent = 0;
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(stop - Clock::now()).count();
        if (left <= 0) return sent;
        const auto timeout = std::min<std::int64_t>(left, std::numeric_limits<int>::max());
        const int ready = ::poll(&fd, 1, static_cast<int>(timeout));
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll replay");
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("replay socket error");
        if (fd.revents & POLLIN) sent += publisher::reply_once(replay.fd(), feed);
    }
}
} // namespace

int main() try {
    using namespace bench;
    auto trial = read_trial();
    const SamplePlan sample(trial);
    std::vector<std::int64_t> lateness;
    lateness.reserve(sample.size());

    // Port zero lets concurrent trials use independent loopback sockets.
    const loopback::Socket live(0), replay(0);
    std::cout << replay.port() << std::endl;
    std::uint16_t a_port{}, b_port{};
    std::int64_t start_ns{};
    if (!(std::cin >> a_port >> b_port >> start_ns)) throw std::runtime_error("missing receiver start");
    const auto a = loopback::address(a_port);
    const auto b = loopback::address(b_port);
    trial.start = Time(std::chrono::nanoseconds(start_ns));

    Sends warmup, measured;
    bool measuring = false;
    publisher::Feed feed(7, 64, [&](Feed leg, const WireMessage& packet) {
        if (leg == Feed::B && !trial.dual) return;
        auto& sends = measuring ? measured : warmup;
        try {
            loopback::send_packet(live.fd(), packet, leg == Feed::A ? &a : &b);
            if (leg == Feed::A) ++sends.a;
            else ++sends.b;
            if (measuring && Clock::now() < trial.end()) ++sends.in_window;
        } catch (const std::system_error&) {
            ++sends.errors;
        }
    });

    // Stop offering new events at the window boundary, even if the sender is behind.
    // The missing attempts then show up in the output instead of extending the trial.
    Count attempted = 0, late_by_one_event = 0;
    std::int64_t max_late_ns = 0;
    for (Count seq = 1; seq <= trial.last_sequence(); ++seq) {
        const auto due = trial.due(seq);
        const auto message = update(seq);
        wait_until_due(due);
        const auto began = Clock::now();
        if (began >= trial.end()) break;
        measuring = seq > trial.warmup_count();
        if (measuring) {
            ++attempted;
            const auto late = ns(began - due);
            max_late_ns = std::max(max_late_ns, late);
            if (began >= trial.due(seq + 1)) ++late_by_one_event;
            if (sample.slot(seq - trial.warmup_count() - 1)) lateness.push_back(late);
        }
        feed.publish(message).value();
    }
    const auto replies = serve_drain(replay, feed, trial.drain_end());

    metric("publish_attempts", attempted);
    metric("sent_a", measured.a);
    metric("sent_b", measured.b);
    metric("sent_in_window", measured.in_window);
    metric("send_errors", measured.errors);
    metric("warmup_send_errors", warmup.errors);
    metric("replay_frames_sent_all", replies);
    metric("starts_at_least_one_event_late", late_by_one_event);
    metric("max_publish_lateness_ns", max_late_ns);
    percentiles("publish_lateness", lateness);
} catch (const std::exception& e) {
    std::cerr << "benchmark publisher: " << e.what() << '\n';
    return 1;
}
