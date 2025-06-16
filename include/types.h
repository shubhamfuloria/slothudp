#ifndef TYPES_H
#define TYPES_H

#include <QVector>

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
    QString speedHint;
};


#endif // TYPES_H
