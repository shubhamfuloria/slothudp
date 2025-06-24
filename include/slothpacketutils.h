#ifndef COMMON_H
#define COMMON_H

#include <QByteArray>

#include <include/types.h>

namespace SlothPacketUtils {

    bool parsePacketHeader(const QByteArray& buffer, PacketHeader& outHeader, QByteArray&  outPayload);
    quint16 calculateChecksum(const QByteArray& data);

    QByteArray serializePacket(const HandshakePacket& packet);
    QByteArray serializePacket( DataPacket& packet);


    HandshakePacket deserializePacket(QByteArray &buffer);
    void deserializePacket(QByteArray &buffer, DataPacket& packet);
    void deserializePacket(QByteArray &buffer, AckWindowPacket& packet);

    QByteArray generateBitmapFromSet(quint32 base, int windowSize, const QSet<quint32>& seqs);


    void deserializePacket(QByteArray &buffer, NackPacket& packet);
    void logBitMap(QByteArray bitmap);
}

#endif // COMMON_H
