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
        DataPacket dataPacket;
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

        case PacketType::FIN:
            qDebug() << "Recieved fin packet, we're safe to close the file now";
            // we should send sender a confirmation packet and tear down connection

            m_file.close();

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
    m_baseAckSeqNum = 0;
    m_baseWriteSeqNum = 0;
    m_untrackedCount = 0;

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

    if(header.sequenceNumber < m_baseWriteSeqNum) {
        qDebug() << "Already written packet " << header.sequenceNumber;
        return;
    }

    m_recvWindow[header.sequenceNumber] = payload;
    m_receivedSeqNums.insert(header.sequenceNumber);

    qDebug() << QString("m_recvWindow size: %1, m_baseWriteSeq: %2, m_untrackedCount: %3")
                    .arg(m_recvWindow.size()).arg(m_baseWriteSeqNum).arg(m_untrackedCount);

    while(m_recvWindow.contains(m_baseWriteSeqNum)) {
        qDebug() << "Writing chunk to file";
        QByteArray chunk = m_recvWindow[m_baseWriteSeqNum];
        m_file.write(chunk);
        m_recvWindow.remove(m_baseWriteSeqNum);
        m_baseWriteSeqNum++;
    }

    m_untrackedCount++;
    if(m_untrackedCount >= 8) {
        sendAcknowledgement();
        m_baseAckSeqNum = m_baseWriteSeqNum;
    }
}

void SlothRxSocket::sendAcknowledgement()
{
    QByteArray bitmap = generateAckBitmap(m_baseAckSeqNum, 8);

    AckWindowPacket packet;
    packet.header.type = PacketType::ACK;
    packet.header.sequenceNumber = 1;
    packet.baseSeqNum = m_baseAckSeqNum;
    packet.bitmapLength = bitmap.size();
    packet.bitmap = bitmap;
    packet.header.payloadSize = bitmap.size() + sizeof(quint8);

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);

    stream << m_baseAckSeqNum
           << static_cast<quint8>(bitmap.size())
           << bitmap;

    packet.header.checksum = qChecksum(payload.constData(), payload.size());

    QByteArray full;
    QDataStream fullStream(&full, QIODevice::WriteOnly);
    fullStream << static_cast<quint8>(packet.header.type)
               << packet.header.sequenceNumber
               << packet.header.payloadSize
               << packet.header.checksum
               << payload;

    writeDatagram(full, m_txAddress, m_txPort);
    qDebug() << "ACK sent for base" << m_baseAckSeqNum << "bitmap size " << bitmap.size();
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

QByteArray SlothRxSocket::generateAckBitmap(quint32 base, int windowSize)
{
    QByteArray bitmap;
    qDebug() << "Generating bitmap, m_receviedSeqNums ";
    qDebug() << m_receivedSeqNums;
    for (int i = 0; i < windowSize; i += 8) {
        quint8 byte = 0;
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i + bit;
            if (m_receivedSeqNums.contains(seq)) {
                byte |= (1 << (7 - bit));
            }
        }
        bitmap.append(byte);
    }
    return bitmap;
}
