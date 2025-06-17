#ifndef TYPES_H
#define TYPES_H

#include <QVector>
#include <QDebug>

enum class PacketType : quint8 {
    DATA = 0,
    ACK = 1,
    NACK = 2,
    HANDSHAKE = 3,
    FIN = 4,
};

#pragma pack(push, 1)
struct PacketHeader {
    PacketType type;
    quint32 sequenceNumber;
    quint16 payloadSize;
    quint32 checksum;

    void print() {
        qDebug() <<
            QString("Packet Type: %1, Seq: %2, payloadSize: %3, checksum: %4")
                        .arg(static_cast<quint8>(type))
                        .arg(sequenceNumber)
                        .arg(payloadSize)
                        .arg(checksum);
    }
};
#pragma pack(pop)


struct DataPacket {
    PacketHeader header;
    QVector<quint8> payload;
};

struct HandshakePacket {
    PacketHeader header;
    QString filename;
    quint64 totalSize;
    quint32 requestId;
    quint8 protocolVersion;
    QString speedHint;
};


#endif // TYPES_H
