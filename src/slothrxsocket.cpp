#include "include/slothrxsocket.h"
#include "include/slothpacketutils.h"

#include <QNetworkDatagram>
#include <QIODevice>
#include <QDataStream>

SlothRxSocket::SlothRxSocket() {


    bool success = bind(5000);

    if(!success) {
        qWarning() << "Could not bind UDP socket of SlotRx";
    }

    connect(this, &QUdpSocket::readyRead, this, &SlothRxSocket::handleReadyRead);
}

void SlothRxSocket::handleReadyRead()
{
    while(hasPendingDatagrams()) {
        QNetworkDatagram datagram = receiveDatagram(4096);
        QByteArray buffer = datagram.data();

        qDebug() << "Received buffer : " << buffer.toHex();
        QByteArray payload;
        PacketHeader header;
        DataPacket dataPacket;
        bool success = SlothPacketUtils::parsePacketHeader(buffer, header, payload);

        if(!success) {
            qWarning() << "SlothRX: Checksum failed, dropping packet";
            continue;
        }
        qDebug() << "payload: " << payload.toHex();

        switch(header.type) {

        case PacketType::HANDSHAKE:
            // on receiving handshake packet, we may want to notify the application for
            // an incoming request by emitting signal on_fileTxRequest()
            // before doing that verify if it's a valid request based on requestId
            // as other party may send multiple duplicate packet of same request (to avoid loss)

            // after validation, set peer info
            // before setting m_txAddress and m_txPort, make sure if you can accept the request
            m_txAddress = datagram.senderAddress();
            m_txPort = datagram.senderPort();
            handleHandshakePacket(payload);
            break;

        case PacketType::DATA:
            // data packet, write it to file

            qDebug() << "SlothRx:: received data packet";
            handlePacket(header, payload);

            break;

        default:
            qDebug() << "SlothRx: Packet type not handled";
        }
    }
}
void SlothRxSocket::handleHandshakePacket(QByteArray buffer)
{
    qDebug() << "handling handshake packet";
    HandshakePacket packet = SlothPacketUtils::deserializePacket(buffer);

    packet.print();

    // notify the application about the request
    emit on_fileTxRequest(packet.filename, packet.totalSize, m_txAddress.toString());


    m_filePath = packet.filename;

    // just for testing we're auto accepting the handshake request
    // in real case this we'll only acknowledge once user triggers the accept

    acknowledgeTxRequest(packet.requestId);
}


bool SlothRxSocket::acknowledgeTxRequest(quint32 requestId)
{
    PacketHeader ack(PacketType::HANDSHAKEACK, 1, 0);
    ack.checksum = 0;

    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream << static_cast<quint8>(ack.type)
           << requestId // send requestId as sequence number
           << 0
           << ack.checksum;

    // at this point, we are acknowledging the request
    // we should prepare now for file writing

    m_file.setFileName(m_filePath);
    if(!m_file.open(OpenModeFlag::Truncate | OpenModeFlag::WriteOnly)) {
        qDebug() << "Could not open file " << m_filePath;
    } else {
        qDebug() << "Opened Successfully file " << m_filePath;
    }

    return transmitBuffer(buffer);
}


void SlothRxSocket::handlePacket(PacketHeader header, QByteArray payload) {
    DataPacket packet;
    SlothPacketUtils::deserializePacket(payload, packet);
    packet.header = header;

    qDebug() << "handle Packet header: ";
    header.print();

    while(header.sequenceNumber == m_baseSeqNum) {
        qDebug() << "Writing chunk to file";
        m_file.write(payload);
        m_baseSeqNum++;
    }

    m_recvWindow[header.sequenceNumber] = payload;


}


bool SlothRxSocket::transmitBuffer(const QByteArray& buffer)
{
    if(m_txAddress.isNull()) {
        qWarning() << "Destination address not set, cannot transmit buffer";
        return false;
    }

    QNetworkDatagram datagram;

    datagram.setData(buffer);
    datagram.setDestination(m_txAddress, m_txPort);
    quint64 bytesSent = writeDatagram(datagram);

    // -1 : failed to write datagram
    return bytesSent != -1;
}
