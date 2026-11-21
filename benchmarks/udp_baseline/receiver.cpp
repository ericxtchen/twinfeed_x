#include "trial.h"
#include "udp_receiver.h"
#include "../../apps/loopback.h"

#include <bit>
#include <cstring>
#include <ctime>

namespace {
using namespace bench;

std::int64_t cpu_ns() {
    timespec t{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) < 0)
        throw std::system_error(errno, std::generic_category(), "receiver CPU clock");
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

int receive_buffer(int fd) {
    int bytes{};
    socklen_t size = sizeof(bytes);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, &size) < 0)
        throw std::system_error(errno, std::generic_category(), "SO_RCVBUF");
    return bytes;
}

struct Reads {
    Count a = 0, b = 0, replay = 0;

    void count(std::optional<Feed> leg) {
        if (!leg) ++replay;
        else if (*leg == Feed::A) ++a;
        else ++b;
    }
};

void print_batch_sizes(std::string_view prefix, const UdpBatchHistogram& bins) {
    Count calls = 0, packets = 0;
    for (std::size_t size = 1; size < bins.size(); ++size) {
        std::cout << prefix << '_' << size << ' ' << bins[size] << '\n';
        calls += bins[size];
        packets += size * bins[size];
    }
    std::cout << prefix << "_calls " << calls << '\n';
    std::cout << prefix << "_packets " << packets << '\n';
    std::cout << prefix << "_mean ";
    if (calls) std::cout << static_cast<double>(packets) / static_cast<double>(calls);
    std::cout << '\n';
}

struct Results {
    const Trial& trial;
    const SamplePlan sample;
    std::vector<Time> first_read;
    std::vector<std::int64_t> scheduled_latency, ingest_latency;
    Reads warmup_reads, reads;
    Count bad_frames = 0, resets = 0, gaps_opened = 0, gaps_closed = 0, requests = 0;
    Count last_update = 0, sequence_errors = 0, warmup_commits = 0, commits = 0, in_window = 0;

    explicit Results(const Trial& t)
        : trial(t), sample(t), first_read(sample.size()),
          scheduled_latency(sample.size(), -1), ingest_latency(sample.size(), -1) {}

    // Count the copy before the arbiter can discard it. Only the v1 sequence field
    // is needed here; the normal receiver still does the full decode and validation.
    void on_read(std::optional<Feed> leg, std::span<const std::byte> packet) {
        if (packet.size() != wire_size) {
            ++bad_frames;
            return;
        }
        Count seq;
        std::memcpy(&seq, packet.data() + 10, sizeof(seq));
        if constexpr (std::endian::native == std::endian::little) seq = std::byteswap(seq);
        if (seq == 0 || seq > trial.last_sequence()) {
            ++bad_frames;
            return;
        }
        if (seq <= trial.warmup_count()) {
            warmup_reads.count(leg);
            return;
        }
        reads.count(leg);
        const auto slot = sample.slot(seq - trial.warmup_count() - 1);
        if (slot && first_read[*slot] == Time{}) first_read[*slot] = Clock::now();
    }

    // One arrival can publish several buffered updates. Each update keeps its own
    // scheduled time and first-read time, even if they all commit in this call.
    void on_notice(const Receiver::Notification& notice) {
        if (std::holds_alternative<Receiver::Reset>(notice)) {
            ++resets;
            return;
        }
        if (const auto* status = std::get_if<Receiver::Status>(&notice)) {
            if (status->state == Receiver::State::Running) {
                if (status->known_gap) ++gaps_opened;
                else ++gaps_closed;
            }
            return;
        }

        const auto seq = std::get<Message>(notice).seq_num;
        if (seq != last_update + 1) ++sequence_errors;
        last_update = seq;
        if (seq <= trial.warmup_count()) {
            ++warmup_commits;
            return;
        }
        ++commits;
        if (seq > trial.last_sequence()) return;
        const auto slot = sample.slot(seq - trial.warmup_count() - 1);
        const auto published = Clock::now();  // End of the counter observer, before recording its timings.
        if (published >= trial.measured_start() && published < trial.end()) ++in_window;
        if (slot) {
            scheduled_latency[*slot] = ns(published - trial.due(seq));
            if (first_read[*slot] != Time{}) ingest_latency[*slot] = ns(published - first_read[*slot]);
        }
    }

    bool received_all() const {
        const auto n = trial.measured_count();
        const auto w = trial.warmup_count();
        return reads.a == n && reads.b == (trial.dual ? n : 0) &&
               warmup_reads.a == w && warmup_reads.b == (trial.dual ? w : 0) &&
               last_update == trial.last_sequence();
    }
};

// A cheap end-of-run check catches a bad book even if the counts happen to match.
bool book_matches(const Receiver& receiver) {
    const auto last = receiver.last_committed_sequence();
    for (Count first = 1; first <= 32; ++first) {
        const auto level = update(first);
        std::optional<Count> quantity;
        if (first <= last) quantity = 1 + (last - first) / 32;
        if (receiver.quantity_at(101, level.side, level.price_ticks) != quantity) return false;
    }
    return true;
}
} // namespace

int main() try {
    using namespace bench;
    auto trial = read_trial();
    std::uint16_t publisher_port{};
    if (!(std::cin >> publisher_port)) throw std::runtime_error("missing publisher port");
    const loopback::Socket a(0), b(0), replay(0);
    const auto peer = loopback::address(publisher_port);
    if (::connect(replay.fd(), reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) < 0)
        throw std::system_error(errno, std::generic_category(), "connect replay");

    Results results(trial);
    Receiver receiver(7, [&](const Receiver&, const Receiver::Notification& n) { results.on_notice(n); });
    UdpReadOptions options;
    options.on_read = [&](auto leg, auto bytes) { results.on_read(leg, bytes); };

    // Both workers use this host's steady clock. The runner forwards this start
    // and the two live ports before the first event comes due.
    trial.start = Clock::now() + 200ms;
    std::cout << a.port() << ' ' << b.port() << ' ' << ns(trial.start.time_since_epoch()) << std::endl;
    auto receive_until = [&](Time end, bool draining) {
        options.wake_by = end;
        while (Clock::now() < end) {
            if (draining && results.requests == 0 && results.received_all()) break;
            const auto request = receive_once(receiver, a.fd(), b.fd(), 10us, replay.fd(), &options);
            if (request) {
                ++results.requests;
                loopback::send_packet(replay.fd(), encode_replay_request(*request).value());
            }
        }
    };

    // Keep servicing sockets across the warm-up boundary. CPU time covers the
    // measured window; the final drain can improve completeness but not commits/sec.
    receive_until(trial.measured_start(), false);
    // Attribute a whole batch to the receive turn that collected it.
    UdpBatchCounts batches;
    options.batch_counts = &batches;
    const auto cpu_start = cpu_ns();
    receive_until(trial.end(), false);
    const auto cpu_used = cpu_ns() - cpu_start;
    options.batch_counts = nullptr;
    receive_until(trial.drain_end(), true);

    print_batch_sizes("batch_a", batches.a);
    print_batch_sizes("batch_b", batches.b);
    metric("read_a", results.reads.a);
    metric("read_b", results.reads.b);
    metric("read_replay", results.reads.replay);
    metric("warmup_read_a", results.warmup_reads.a);
    metric("warmup_read_b", results.warmup_reads.b);
    metric("warmup_read_replay", results.warmup_reads.replay);
    metric("unexpected_frames_all", results.bad_frames);
    metric("warmup_commits", results.warmup_commits);
    metric("commits_in_window", results.in_window);
    metric("commits_after_drain", results.commits);
    metric("last_sequence", receiver.last_committed_sequence());
    metric("sequence_errors_all", results.sequence_errors);
    metric("gaps_opened_all", results.gaps_opened);
    metric("gaps_closed_all", results.gaps_closed);
    metric("gap_at_end", receiver.known_gap());
    metric("replay_requests_all", results.requests);
    metric("resets_all", results.resets);
    const auto state = receiver.state();
    metric("state", state == Receiver::State::Running ? "running" :
                    state == Receiver::State::NeedsRecovery ? "needs_recovery" : "error");
    metric("book_ok", book_matches(receiver));
    metric("receiver_cpu_window_ns", cpu_used);
    metric("a_rcvbuf_bytes", receive_buffer(a.fd()));
    metric("b_rcvbuf_bytes", receive_buffer(b.fd()));
    percentiles("scheduled_to_observer", results.scheduled_latency);
    percentiles("ingest_to_observer", results.ingest_latency);
} catch (const std::exception& e) {
    std::cerr << "benchmark receiver: " << e.what() << '\n';
    return 1;
}
