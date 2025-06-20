#ifndef TYPES_H
#define TYPES_H

#include <QVector>
#include <QDebug>
#include <QDataStream>

#include <QMap>

enum class PacketType : quint8 {
    DATA = 0,
    ACK = 1,
    NACK = 2,
    HANDSHAKE = 3,
    HANDSHAKEACK = 4,
    FIN = 5,
    BYE = 6
};

#pragma pack(push, 1)
struct PacketHeader {
    PacketType type;
    quint32 sequenceNumber;
    quint16 payloadSize;
    quint32 checksum;


    PacketHeader() {}
    PacketHeader(PacketType type, quint32 sequenceNumber, quint16 payloadSize) {
        this->type = type;
        this->sequenceNumber = sequenceNumber;
        this->payloadSize = payloadSize;
    }
    void print() {

        QMap<int,QString> names = {{0, "DATA"}, {1, "ACK"}, {2, "NACK"}, {3, "HANDSHAKE"},
                                        {4, "HANDSHAKEACK"}, {5, "FIN"}, {6, "BYE"}};
        qDebug() <<
            QString("Packet Type: %1, Seq: %2, payloadSize: %3, checksum: %4")
                        .arg(names[static_cast<int>(type)])
                        .arg(sequenceNumber)
                        .arg(payloadSize)
                        .arg(checksum);
    }

    QByteArray serialize() {
        QByteArray buffer;
        QDataStream stream(&buffer, QIODevice::WriteOnly);

        stream << static_cast<quint8>(type)
               << sequenceNumber
               << payloadSize
               << checksum;

        return buffer;
    }
};
#pragma pack(pop)


struct DataPacket {
    PacketHeader header;
    QByteArray chunk;
};

struct HandshakePacket {
    PacketHeader header;
    QString filename;
    quint64 totalSize;
    quint32 requestId;
    quint8 protocolVersion;
    QString speedHint;


    void print() {
        qDebug() <<
            QString("HANDSHAKE:: fileName: %1, totalSize: %2, requestId: %3, protocolVersion: %4")
                        .arg(filename).arg(totalSize).arg(requestId).arg(protocolVersion);
    }

};

struct AckWindowPacket {
    PacketHeader header;
    quint32 baseSeqNum;
    quint8 bitmapLength;
    QByteArray bitmap;

    QByteArray serialize() {
        QByteArray buffer;
        QDataStream stream(&buffer, QIODevice::WriteOnly);

        stream << header.serialize();
        stream << baseSeqNum
               << bitmapLength
               << bitmap;

      return buffer;
    }

    void print() {
        qDebug() << QString("baseSeq: %1, bitMapLength: %2, bitmap: ")
                        .arg(baseSeqNum).arg(bitmapLength);
        qDebug() << bitmap;
    }
};

// struct NackPacket {
//     PacketHeader header;
//     QList<quint32> missingSeqNum;

//     QByteArray serialize() {
//         QByteArray buffer;
//         QDataStream stream(&buffer, QIODevice::WriteOnly);

//         stream << header.serialize();
//         stream << missingSeqNum;

//         return buffer;
//     }
// };

struct NackPacket {
    PacketHeader header;
    quint32 baseSeqNum;
    quint8 bitmapLength;         // number of bytes in the bitmap
    QByteArray bitmap;           // bits: 1 = missing, 0 = received


    NackPacket(quint32 baseSeq, quint8 bitMapLength, QByteArray bitmap) {
        this->baseSeqNum = baseSeq;
        this->bitmapLength = bitMapLength;
        this->bitmap = bitmap;
    }

    QByteArray serialize() {
        QByteArray buffer;
        QDataStream stream(&buffer, QIODevice::WriteOnly);

        stream << header.serialize();
        stream << baseSeqNum
               << bitmapLength
               << bitmap;

        return buffer;
    }
};


#endif // TYPES_H
