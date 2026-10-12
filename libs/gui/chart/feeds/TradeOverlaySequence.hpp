#pragma once

namespace chart {
// The worker must stop while its event loop is alive, before disconnecting and
// joining. Pointer-like owners keep the same ordering for real workers and fakes.
template <class Processor, class Thread, class Stop, class Disconnect>
void teardownTradeOverlayFeeds(Processor& processor, Thread& thread, Stop stop, Disconnect disconnect) {
    if (processor) {
        if (thread && thread->isRunning()) {
            stop(processor.get());
        } else {
            processor->stopProcessing();
        }
        disconnect(processor.get());
    }
    if (thread && thread->isRunning()) {
        thread->quit();
        if (!thread->wait(5000)) {
            thread->terminate();
            thread->wait(1000);
        }
    }
    processor.reset();
    thread.reset();
}

template <class Processor, class Footprint, class Vp, class QueueClear>
void clearTradeOverlayFeeds(Processor& processor, Footprint& footprint, Vp& vp, QueueClear queueClear) {
    if (processor) queueClear(processor.get());
    footprint.clearPending();
    vp.clearPending();
    footprint.requestNeutralReset();
}
} // namespace chart
