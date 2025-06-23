#include <QDataStream>

#include "include/slothpacketutils.h"


namespace SlothPacketUtils {

quint16 calculateChecksum(const QByteArray& data)
{
    return qChecksum(data.constData(), data.size());
}

bool parsePacketHeader(const QByteArray& buffer, PacketHeader& outHeader, QByteArray& outPayload)
{
    const int HEADER_SIZE = sizeof(PacketHeader);  // 11
    if (buffer.size() < HEADER_SIZE) return false;

    QDataStream stream(buffer);
    quint8 typeByte;
    stream >> typeByte;
    outHeader.type = static_cast<PacketType>(typeByte);

    stream >> outHeader.sequenceNumber
        >> outHeader.payloadSize
        >> outHeader.headerChecksum
        >> outHeader.checksum;

    outHeader.print();
    outPayload = buffer.mid(PACKET_HEADER_SIZE, outHeader.payloadSize);
    quint16 calculatedHeaderCheckSum = qChecksum(buffer, 7);
    if (calculatedHeaderCheckSum != outHeader.headerChecksum) {
        qDebug() << QString("header checksum didn't match ( calculated : %1, received: %2 ), dropping packet")
                        .arg(calculatedHeaderCheckSum)
                        .arg(outHeader.headerChecksum);
        return false;
    }

    if (buffer.size() < PACKET_HEADER_SIZE + outHeader.payloadSize) {
        return false;
    }

    quint16 calculated = calculateChecksum(outPayload);
    return calculated == outHeader.checksum;
}


QByteArray serializePacket(const HandshakePacket& packet)
{
    QByteArray payload;
    QDataStream payloadStream(&payload, QIODevice::WriteOnly);

    payloadStream << packet.filename
                  << packet.totalSize
                  << packet.requestId
                  << packet.protocolVersion
                  << packet.speedHint;

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);

    PacketHeader header;
    header.type = PacketType::HANDSHAKE;
    header.sequenceNumber = 0;
    header.payloadSize = payload.size();

    stream << static_cast<quint8>(header.type)
           << header.sequenceNumber
           << header.payloadSize;

    header.headerChecksum = qChecksum(buffer.constData(), 7);
    header.checksum = qChecksum(payload.constData(), payload.size());

    stream << header.headerChecksum
           << header.checksum;

    stream.writeRawData(payload.constData(), payload.size());

    return buffer;
}


QByteArray serializePacket(DataPacket& packet)
{
    packet.header.checksum = qChecksum(packet.chunk.constData(), packet.chunk.size());
    QByteArray headerBytes = packet.header.serialize(packet.header.checksum);

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream.writeRawData(headerBytes.constData(), headerBytes.size());
    stream.writeRawData(packet.chunk.constData(), packet.chunk.size());

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

    quint32 baseSeqNum;
    quint8 bitmapLength;

    stream >> baseSeqNum;

    // Read 1 byte manually for bitmapLength
    char lenByte;
    stream.readRawData(&lenByte, 1);
    bitmapLength = static_cast<quint8>(lenByte);

    packet.baseSeqNum = baseSeqNum;
    packet.bitmapLength = bitmapLength;

    packet.bitmap.resize(bitmapLength);
    stream.readRawData(packet.bitmap.data(), bitmapLength);

    QString bitString;
    for (int i = 0; i < packet.bitmap.size(); ++i) {
        quint8 byte = static_cast<quint8>(packet.bitmap[i]);
        bitString += QString("%1").arg(byte, 8, 2, QChar('0')) + " ";
    }
}

void deserializePacket(QByteArray &buffer, NackPacket& packet)
{
    QDataStream stream(&buffer, QIODevice::ReadOnly);

    quint32 baseSeqNum;
    quint8 bitmapLength;

    stream >> baseSeqNum;

    // Read 1 byte manually for bitmapLength
    char lenByte;
    stream.readRawData(&lenByte, 1);
    bitmapLength = static_cast<quint8>(lenByte);

    packet.baseSeqNum = baseSeqNum;
    packet.bitmapLength = bitmapLength;

    packet.bitmap.resize(bitmapLength);
    stream.readRawData(packet.bitmap.data(), bitmapLength);
}


QByteArray generateBitmapFromSet(quint32 base, int windowSize, const QSet<quint32>& relevantSeqNums)
{
    QByteArray bitmap;
    for (int i = 0; i < windowSize; i += 8) {
        quint8 byte = 0;
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i + bit;
            if (relevantSeqNums.contains(seq)) {
                byte |= (1 << (7 - bit));
            }
        }
        bitmap.append(byte);
    }
    return bitmap;
}

}
