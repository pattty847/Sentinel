#include "RestartJournal.hpp"
#include "capture/RawCapture.hpp"
using nlohmann::json;
using namespace sentinel;
struct RestartJournal::Impl {
    std::filesystem::path root;
    capture::Writer writer;
    int second = -60;
    explicit Impl(const std::filesystem::path& root, int start) : root(root), writer(config(root),
        {{"product_metadata",{{"product_id","BTC-USD"},{"quote_increment","0.01"},{"base_increment","0.00000001"}}}}) {
        second=start;
        append(capture::Kind::TransportUp,"{}");
        append(capture::Kind::Frame,frame("snapshot",2));
    }
    static capture::WriterConfig config(const std::filesystem::path& p) {
        capture::WriterConfig c; c.root=QString::fromStdString(p.string()); c.fsyncBlocks=0; return c;
    }
    std::string frame(const char* type, int quantity) {
        return json({{"channel","l2_data"},{"events",json::array({{{"type",type},{"product_id","BTC-USD"},
            {"updates",json::array({{{"side","bid"},{"price_level","99999.99"},{"new_quantity",std::to_string(quantity)}},
                {{"side","offer"},{"price_level","100000.01"},{"new_quantity","3"}}})}}})}}).dump();
    }
    void append(capture::Kind kind, std::string payload) {
        writer.append({kind,{(RestartJournal::Epoch+int64_t(second)*1000)*1'000'000,int64_t(second+60)*1'000'000},1,std::move(payload)});
    }
};
RestartJournal::RestartJournal(const std::filesystem::path& p, bool warmup) : impl(std::make_unique<Impl>(p,warmup ? -60 : 0)) {}
RestartJournal::~RestartJournal() = default;
void RestartJournal::grow(int minutes) {
    while (impl->second < minutes*60+5) {
        impl->second+=5;
        impl->append(capture::Kind::Frame,impl->frame("update",2+((impl->second/5)%7+7)%7));
    }
    impl->writer.flush();
}
void RestartJournal::restart(int second) {
    const auto root=impl->root;
    impl.reset();
    impl=std::make_unique<Impl>(root,second);
}

void RestartJournal::at(int second) {
    impl->second=second;
    impl->append(capture::Kind::Frame,impl->frame("update",2+((second/5)%7+7)%7));
    impl->writer.flush();
}
