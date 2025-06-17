#include <QDataStream>

#include "include/slothpacketutils.h"


namespace SlothPacketUtils {

quint16 calculateChecksum(const QByteArray& data)
{
    return qChecksum(data.constData(), data.size());
}

bool parsePacketHeader(const QByteArray& buffer, PacketHeader& outHeader, QByteArray& outPayload)
{
    if (buffer.size() < 9) return false;

    QDataStream stream(buffer);
    quint8 typeByte;
    stream >> typeByte;
    outHeader.type = static_cast<PacketType>(typeByte);

    stream >> outHeader.sequenceNumber
        >> outHeader.payloadSize
        >> outHeader.checksum
        >> outPayload;

    int headerSize = 11;

    if (buffer.size() < headerSize + outHeader.payloadSize) {
        return false;
    }
    quint16 calculated = calculateChecksum(outPayload);
    return calculated == outHeader.checksum;
}

}
