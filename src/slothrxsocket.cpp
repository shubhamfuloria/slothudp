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

        QByteArray payload;
        PacketHeader header;
        bool success = SlothPacketUtils::parsePacketHeader(buffer, header, payload);

        if(!success) {
            qWarning() << "SlothRX: Checksum failed, dropping packet";
            continue;
        }

        switch(header.type) {

        case PacketType::HANDSHAKE:
            // on receiving handshake packet, we may want to notify the application for
            // an incoming request by emitting signal on_fileTxRequest()
            // before doing that verify if it's a valid request based on requestId
            // as other party may send multiple duplicate packet of same request (to avoid loss)

            // after validation, set peer info

            m_txAddress = datagram.senderAddress();
            m_txPort = datagram.senderPort();
            acknowledgeTxRequest(999);
            break;


        default:
            qDebug() << "SlothRx: Packet type not handled";
        }
    }
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

    return transmitBuffer(buffer);
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
