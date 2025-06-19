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

QByteArray serializePacket(const HandshakePacket& packet)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << packet.filename
           << packet.totalSize
           << packet.requestId
           << packet.protocolVersion;


    // add header at the top of the packet
    PacketHeader header;
    header.type = PacketType::HANDSHAKE;
    header.sequenceNumber = 0;
    header.payloadSize = payload.size();
    header.checksum = qChecksum(payload.constData(), payload.size());

    QByteArray mainBuffer;
    QDataStream mainStream(&mainBuffer, QIODevice::WriteOnly);

    mainStream << static_cast<quint8>(header.type)
               << header.sequenceNumber
               << header.payloadSize
               << header.checksum
               << payload;

    return mainBuffer;
}

QByteArray serializePacket(const DataPacket& packet)
{

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);

    stream << static_cast<quint8>(packet.header.type)
           << packet.header.sequenceNumber
           << packet.header.payloadSize
           << packet.header.checksum;



    // serialize chunk
    stream << packet.chunk;
    return buffer;
}


HandshakePacket deserializePacket(QByteArray &buffer)
{
    HandshakePacket packet;
    QDataStream stream(&buffer, QIODevice::ReadOnly);

    stream >> packet.filename
        >> packet.totalSize
        >> packet.requestId
        >> packet.protocolVersion;

    return packet;
}

void deserializePacket(QByteArray &buffer, DataPacket& packet)
{
    QDataStream stream(&buffer, QIODevice::ReadOnly);
    packet.chunk = buffer;
}

void deserializePacket(QByteArray &buffer, AckWindowPacket& packet)
{
    QDataStream stream(&buffer, QIODevice::ReadOnly);
    stream >> packet.baseSeqNum
            >> packet.bitmapLength
        >> packet.bitmap;
}
}
