#include "core/gzip.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/crc32.h"
#include "miniz.h"

namespace tw {

namespace {

constexpr size_t kPiece = 1 << 20;

inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

}  // namespace

// ---- writer ---------------------------------------------------------------------------------------

struct GzipWriter::Impl {
    struct Job {
        std::vector<uint8_t> in, out;
        uint32_t crc = 0;
        bool last = false, done = false, ok = false;
    };

    WriteFn out;
    int level;
    unsigned nthreads;
    size_t maxInFlight;
    std::vector<std::thread> workers;
    std::mutex m;
    std::condition_variable work, finished;
    std::deque<Job*> todo;                    // waiting for a worker
    std::deque<std::unique_ptr<Job>> order;   // handed to the workers and not written yet, in input order
    std::vector<std::unique_ptr<Job>> spare;  // written, buffers kept for reuse
    std::unique_ptr<Job> cur;                 // being filled
    bool stop = false, failed = false, header = false, closed = false;
    uint32_t crc = 0;
    uint64_t total = 0;

    void worker() {
        mz_stream zs{};
        bool init = false;
        for (;;) {
            Job* j;
            {
                std::unique_lock<std::mutex> lk(m);
                work.wait(lk, [&] { return stop || !todo.empty(); });
                if (todo.empty()) break;
                j = todo.front();
                todo.pop_front();
            }
            bool ok = compress(zs, init, *j);
            {
                std::lock_guard<std::mutex> lk(m);
                j->ok = ok;
                j->done = true;
            }
            finished.notify_all();
        }
        if (init) mz_deflateEnd(&zs);
    }

    // One piece: raw deflate ended by a sync flush (byte-aligned, not final), or by the final block for the last one.
    bool compress(mz_stream& zs, bool& init, Job& j) {
        j.crc = crc32::update(0, j.in.data(), j.in.size());
        if (!init) {
            memset(&zs, 0, sizeof zs);
            if (mz_deflateInit2(&zs, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) return false;
            init = true;
        } else if (mz_deflateReset(&zs) != MZ_OK) {
            return false;
        }
        j.out.resize(size_t(mz_deflateBound(&zs, mz_ulong(j.in.size()))) + 64);
        zs.next_in = j.in.data();
        zs.avail_in = unsigned(j.in.size());
        size_t have = 0;
        for (;;) {
            zs.next_out = j.out.data() + have;
            zs.avail_out = unsigned(j.out.size() - have);
            int r = mz_deflate(&zs, j.last ? MZ_FINISH : MZ_SYNC_FLUSH);
            have = j.out.size() - zs.avail_out;
            if (j.last ? r == MZ_STREAM_END : (r == MZ_OK && zs.avail_in == 0 && zs.avail_out > 0)) break;
            if (r != MZ_OK && r != MZ_BUF_ERROR) return false;
            j.out.resize(j.out.size() * 2);  // not enough room (never with the bound): go on with more
        }
        j.out.resize(have);
        return true;
    }

    std::unique_ptr<Job> take() {
        std::lock_guard<std::mutex> lk(m);
        if (spare.empty()) {
            auto j = std::make_unique<Job>();
            j->in.reserve(kPiece);
            return j;
        }
        auto j = std::move(spare.back());
        spare.pop_back();
        return j;
    }

    bool emit(const Job& j) {
        if (!header) {
            const uint8_t h[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
            if (!out(h, sizeof h)) return false;
            header = true;
        }
        crc = crc32::combine(crc, j.crc, j.in.size());
        return j.out.empty() || out(j.out.data(), j.out.size());
    }

    // Writes the pieces that are ready, in order; waits while more than `keep` are still in flight.
    bool drain(size_t keep) {
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
            while (!order.empty() && order.front()->done) {
                std::unique_ptr<Job> j = std::move(order.front());
                order.pop_front();
                lk.unlock();
                bool ok = j->ok && emit(*j);
                j->in.clear();
                lk.lock();
                spare.push_back(std::move(j));
                if (!ok) return false;
            }
            if (order.size() <= keep) return true;
            finished.wait(lk, [&] { return order.front()->done; });
        }
    }

    bool submit(bool last) {
        cur->last = last;
        cur->done = cur->ok = false;
        {
            std::lock_guard<std::mutex> lk(m);
            todo.push_back(cur.get());
            order.push_back(std::move(cur));
        }
        work.notify_one();
        if (!drain(last ? 0 : maxInFlight)) return failed = true, false;
        return true;
    }
};

unsigned GzipWriter::defaultThreads() {
    unsigned n = std::thread::hardware_concurrency();
    return std::max(1u, std::min(n, 8u));
}

GzipWriter::GzipWriter(WriteFn out, int level, unsigned threads) : d_(new Impl) {
    d_->out = std::move(out);
    d_->level = level;
    d_->nthreads = threads ? threads : defaultThreads();
    d_->maxInFlight = size_t(d_->nthreads) * 2;
    for (unsigned i = 0; i < d_->nthreads; i++) d_->workers.emplace_back([this] { d_->worker(); });
}

GzipWriter::~GzipWriter() {
    {
        std::lock_guard<std::mutex> lk(d_->m);
        d_->todo.clear();  // nothing more is written: pieces not started are dropped
        d_->stop = true;
    }
    d_->work.notify_all();
    for (auto& t : d_->workers) t.join();
}

unsigned GzipWriter::threads() const { return d_->nthreads; }

bool GzipWriter::write(const void* data, size_t n) {
    Impl& d = *d_;
    if (d.failed || d.closed) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n) {
        if (!d.cur) d.cur = d.take();
        size_t k = std::min(n, kPiece - d.cur->in.size());
        d.cur->in.insert(d.cur->in.end(), p, p + k);
        p += k;
        n -= k;
        d.total += k;
        if (d.cur->in.size() == kPiece && !d.submit(false)) return false;
    }
    return true;
}

bool GzipWriter::finish() {
    Impl& d = *d_;
    if (d.failed || d.closed) return false;
    d.closed = true;
    if (!d.cur) d.cur = d.take();
    if (!d.submit(true)) return false;
    uint8_t t[8];
    for (int i = 0; i < 4; i++) t[i] = uint8_t(d.crc >> (8 * i));
    for (int i = 0; i < 4; i++) t[4 + i] = uint8_t(uint32_t(d.total) >> (8 * i));  // the length mod 2^32
    return d.out(t, sizeof t);
}

// ---- reader ---------------------------------------------------------------------------------------

struct GunzipReader::Impl {
    ReadFn in;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1 << 18);
    size_t pos = 0, len = 0;
    bool eof = false, inMember = false, zinit = false, done = false, failed = false;
    mz_stream zs{};
    uint32_t crc = 0;
    uint64_t size = 0;
    int members = 0;
    std::string err;

    bool fail(const std::string& e) {
        if (!failed) err = "gzip: " + e;
        failed = true;
        return false;
    }
    bool fill() {  // false: no more input (or a read error, which fails)
        if (pos < len) return true;
        if (eof) return false;
        ssize_t r = in(buf.data(), buf.size());
        if (r <= 0) {
            eof = true;
            if (r < 0) fail("read error");
            return false;
        }
        pos = 0;
        len = size_t(r);
        return true;
    }
    bool byte(uint8_t& b) {
        if (!fill()) return false;
        b = buf[pos++];
        return true;
    }
    bool skipZ() {  // a zero-terminated name or comment
        uint8_t b;
        do
            if (!byte(b)) return false;
        while (b);
        return true;
    }

    // At a member boundary: the end of the input, zero padding, or the next member.
    bool startMember() {
        if (!fill()) {
            if (failed) return false;
            if (members == 0) return fail("empty file");
            done = true;
            return false;
        }
        if (members > 0 && buf[pos] == 0) {  // zero padding after the last member
            while (fill()) {
                for (; pos < len; pos++)
                    if (buf[pos]) return fail("data after the end of the gzip data");
            }
            if (failed) return false;
            done = true;
            return false;
        }
        uint8_t h[10];
        for (size_t k = 0; k < sizeof h; k++) {
            if (!byte(h[k])) return fail(members && k < 2 ? "data after the end of the gzip data" : "the file is cut off (header)");
            if (k == 1 && (h[0] != 0x1f || h[1] != 0x8b)) return fail(members ? "data after the end of the gzip data" : "not a gzip file");
        }
        if (h[2] != 8) return fail("unknown compression method");
        uint8_t flg = h[3];
        if (flg & 0xE0) return fail("damaged header (reserved flags)");
        if (flg & 4) {
            uint8_t a, b;
            if (!byte(a) || !byte(b)) return fail("the file is cut off (header)");
            for (size_t k = size_t(a | b << 8); k; k--)
                if (!byte(a)) return fail("the file is cut off (header)");
        }
        if ((flg & 8) && !skipZ()) return fail("the file is cut off (header)");
        if ((flg & 16) && !skipZ()) return fail("the file is cut off (header)");
        if (flg & 2) {
            uint8_t a;
            if (!byte(a) || !byte(a)) return fail("the file is cut off (header)");
        }
        if (!zinit) {
            memset(&zs, 0, sizeof zs);
            if (mz_inflateInit2(&zs, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return fail("out of memory");
            zinit = true;
        } else if (mz_inflateReset(&zs) != MZ_OK) {
            return fail("out of memory");
        }
        crc = 0;
        size = 0;
        inMember = true;
        return true;
    }

    bool endMember() {
        uint8_t t[8];
        for (uint8_t& c : t)
            if (!byte(c)) return fail("the file is cut off (trailer)");
        if (le32(t) != crc) return fail("damaged data (CRC mismatch)");
        if (le32(t + 4) != uint32_t(size)) return fail("damaged data (length mismatch)");
        members++;
        inMember = false;
        return true;
    }
};

GunzipReader::GunzipReader(ReadFn in) : d_(new Impl) { d_->in = std::move(in); }

GunzipReader::~GunzipReader() {
    if (d_->zinit) mz_inflateEnd(&d_->zs);
}

const std::string& GunzipReader::error() const { return d_->err; }
int GunzipReader::members() const { return d_->members; }

ssize_t GunzipReader::read(void* out, size_t n) {
    Impl& d = *d_;
    if (n == 0) return 0;
    while (!d.failed && !d.done) {
        if (!d.inMember) {
            if (!d.startMember()) break;
            continue;
        }
        if (!d.fill()) {
            if (!d.failed) d.fail("the file is cut off");
            break;
        }
        size_t before = d.pos;
        d.zs.next_in = d.buf.data() + d.pos;
        d.zs.avail_in = unsigned(d.len - d.pos);
        d.zs.next_out = static_cast<uint8_t*>(out);
        d.zs.avail_out = unsigned(std::min<size_t>(n, 1u << 30));
        int r = mz_inflate(&d.zs, MZ_NO_FLUSH);
        d.pos = d.len - d.zs.avail_in;
        size_t got = size_t(d.zs.next_out - static_cast<uint8_t*>(out));
        d.crc = crc32::update(d.crc, out, got);
        d.size += got;
        if (r == MZ_STREAM_END) {
            if (!d.endMember()) break;
        } else if ((r != MZ_OK && r != MZ_BUF_ERROR) || (got == 0 && d.pos == before)) {
            d.fail("damaged data");
            break;
        }
        if (got) return ssize_t(got);
    }
    return d.failed ? -1 : 0;
}

}  // namespace tw
