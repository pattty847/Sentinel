#pragma once
// Flow decoding is the edge of the payload-neutral SCK1 control plane. Replies
// leave the bounded client decode worker with immutable, single-allocation data.
#include "ChunkClientWire.hpp"
#include <QMetaType>
#include <QString>
namespace protocol::chunks {
struct Reply {
    Frame frame; // payload released after decode
    std::shared_ptr<const sentinel::flow::chunks::FlowChunk> body;
    size_t wireBytes = 0;
};
using ReplyPtr = std::shared_ptr<const Reply>;
struct Failure { quint64 requestId = 0; std::optional<Key> key; QString code, message; };
}
Q_DECLARE_METATYPE(protocol::chunks::Key)
Q_DECLARE_METATYPE(protocol::chunks::Availability)
Q_DECLARE_METATYPE(protocol::chunks::ReplyPtr)
Q_DECLARE_METATYPE(protocol::chunks::Failure)
