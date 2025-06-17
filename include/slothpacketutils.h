#ifndef COMMON_H
#define COMMON_H

#include <QByteArray>

#include <include/types.h>

namespace SlothPacketUtils {

    bool parsePacketHeader(const QByteArray& buffer, PacketHeader& outHeader, QByteArray&  outPayload);

quint16 calculateChecksum(const QByteArray& data);

}

#endif // COMMON_H
