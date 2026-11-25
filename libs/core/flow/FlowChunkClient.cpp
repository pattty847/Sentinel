#include "FlowChunkClient.hpp"
#include <stdexcept>
namespace sentinel::flow {
FlowChunkClient::FlowChunkClient(Factory factory, size_t budget) : cache_(budget) {
    if (!factory) throw std::invalid_argument("chunk transport factory missing");
    context_ = new QObject; thread_.setObjectName(QStringLiteral("flow-chunks"));
    context_->moveToThread(&thread_);
    QObject::connect(&thread_, &QThread::finished, context_, &QObject::deleteLater);
    thread_.start(); std::exception_ptr failure;
    QMetaObject::invokeMethod(context_, [&] {
        try {
            transport_ = factory(context_);
            if (!transport_ || transport_->thread() != &thread_) throw std::invalid_argument("invalid chunk transport thread");
            fetcher_ = new FlowChunkFetcher(cache_, *transport_, context_);
        } catch (...) { failure = std::current_exception(); }
    }, Qt::BlockingQueuedConnection);
    if (failure) { thread_.quit(); thread_.wait(); std::rethrow_exception(failure); }
}
FlowChunkClient::~FlowChunkClient() {
    Q_ASSERT(QThread::currentThread() != &thread_);
    QMetaObject::invokeMethod(context_, [this] { delete fetcher_; delete transport_; }, Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait();
}
void FlowChunkClient::queued(std::function<void(FlowChunkFetcher&)> work) {
    QMetaObject::invokeMethod(context_, [this, work = std::move(work)] { work(*fetcher_); }, Qt::QueuedConnection);
}
}
