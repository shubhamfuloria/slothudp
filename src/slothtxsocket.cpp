#include "include/slothtxsocket.h"

#include <QRandomGenerator>
#include <QIODevice>
#include <QDataStream>
#include <QFileInfo>

SlothTxSocket::SlothTxSocket() {}


void SlothTxSocket::initiateHandshake(
    const QString &filePath,
    qint64 fileSize,
    QString destination,
    quint16 port)
{
    qInfo() << QString("Initiating handshake with %1:%2, sending file %3 of size %4")
                   .arg(destination)
                   .arg(port).arg(filePath)
                   .arg(fileSize);

    m_filePath = filePath;
    m_fileSize = fileSize;
    m_destAddress = destination;
    m_destPort = port;

    // generate handshake packet
    HandshakePacket packet;
    packet.filename = QFileInfo(filePath).fileName();
    packet.totalSize = fileSize;
    packet.requestId = QRandomGenerator::global()->generate();
    packet.protocolVersion = 1;

    QByteArray buffer = serializePacket(packet);
    // send the buffer over to network
}


QByteArray SlothTxSocket::serializePacket(const HandshakePacket& packet)
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
