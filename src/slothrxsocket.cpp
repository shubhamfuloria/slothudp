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
    connect(&m_nackTimer, &QTimer::timeout, this, &SlothRxSocket::handleNackTimeout);
}

void SlothRxSocket::handleReadyRead()
{
    while(hasPendingDatagrams()) {
        qDebug() << "RX received";
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

    /*
     * check if it's a duplicate handshake request, if so ignore it
     * if it's a new handshake request, then reject it
    */

    if (m_sessionState != SessionState::NOTACTIVE) {
        if (packet.requestId == m_activeSessionId) {
            qDebug() << "Duplicate handshake packet received. Ignoring.";
        } else {
            qDebug() << "Another session is already active. Rejecting new handshake.";
        }
        return;
    }

    packet.print();

    /*
     * We've successfully received the handshake packet, so application make this
     * request id as session id with pending state.
     * once application acknowledges the handshake request, the state gets changed
     * to ACTIVE state
     *
    */

    m_activeSessionId = packet.requestId;
    m_sessionState = SessionState::REQPENDING;

    // notify the application about the request
    emit on_fileTxRequest(packet.filename, packet.totalSize, m_txAddress.toString());

    // reset to initial state
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
    // prepare packet
    PacketHeader ack(PacketType::HANDSHAKEACK, requestId, 0);
    QByteArray buffer = ack.serialize(0); // no payload, hence 0 checksum

    // at this point, we are acknowledging the request
    // we should prepare now for file writing

    m_file.setFileName(m_filePath);
    if(!m_file.open(OpenModeFlag::Truncate | OpenModeFlag::WriteOnly)) {
        qDebug() << "Could not open file " << m_filePath;
    } else {
        qDebug() << "Opened Successfully file " << m_filePath;
    }

    /*
     * Start NACK Timer
     *
    */

    // // m_nackTimer.start(500);
    return transmitBuffer(buffer);
}


void SlothRxSocket::handlePacket(PacketHeader header, QByteArray payload) {

    if(header.sequenceNumber < m_baseWriteSeqNum) {
        qDebug() << "Already written packet " << header.sequenceNumber;
        return;
    }
    m_highestSeqReceived = m_highestSeqReceived < header.sequenceNumber ? header.sequenceNumber : m_highestSeqReceived;

    m_recvWindow[header.sequenceNumber] = payload;
    m_receivedSeqNums.insert(header.sequenceNumber);

    // qDebug() << QString("m_recvWindow size: %1, m_baseWriteSeq: %2, m_untrackedCount: %3")
    //                 .arg(m_recvWindow.size()).arg(m_baseWriteSeqNum).arg(m_untrackedCount);

    while(m_recvWindow.contains(m_baseWriteSeqNum)) {
        QByteArray chunk = m_recvWindow[m_baseWriteSeqNum];
        m_file.write(chunk);
        m_recvWindow.remove(m_baseWriteSeqNum);
        m_baseWriteSeqNum++;
    }

    m_untrackedCount++;
    if(m_untrackedCount >= 8) {
        sendAcknowledgement();
        while (m_receivedSeqNums.contains(m_baseAckSeqNum)) {
            ++m_baseAckSeqNum;
        }
        m_untrackedCount = 0;
        m_baseAckSeqNum = m_baseWriteSeqNum;
    }
}

void SlothRxSocket::sendAcknowledgement()
{
    QByteArray bitmap = generateAckBitmap(m_baseAckSeqNum, 8);

    AckWindowPacket packet;
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << packet.baseSeqNum
           << static_cast<quint8>(bitmap.size());
    stream.writeRawData(bitmap.constData(), bitmap.size());

    packet.header = PacketHeader(PacketType::ACK, 1, payload.size());
    packet.baseSeqNum = m_baseAckSeqNum;
    packet.bitmapLength = bitmap.size();
    packet.bitmap = bitmap;
    packet.header.checksum = qChecksum(payload.constData(), payload.size());

    QByteArray fullBuffer;
    QDataStream fullStream(&fullBuffer, QIODevice::WriteOnly);

    // write header
    fullStream << static_cast<quint8>(packet.header.type)
           << packet.header.sequenceNumber
           << packet.header.payloadSize;

    packet.header.headerChecksum = qChecksum(fullBuffer.constData(), 7);
    fullStream << packet.header.headerChecksum
           << packet.header.checksum;

    fullStream.writeRawData(payload.constData(), payload.size());
    transmitBuffer(fullBuffer);
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
    for (int i = 0; i < windowSize; i += 8) {
        quint8 byte = 0;
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i + bit;
            bool received = m_receivedSeqNums.contains(seq);

            if (received) {
                byte |= (1 << (7 - bit));  // MSB-first
            }
        }
        bitmap.append(byte);
    }
    return bitmap;
}

void SlothRxSocket::handleNackTimeout()
{
    QSet<quint32> currentMissing;

    for (quint32 i = m_baseAckSeqNum; i <= m_highestSeqReceived; ++i) {
        if (!m_receivedSeqNums.contains(i)) {
            currentMissing.insert(i);
        }
    }
    qDebug() << "Nack Timeout current missing: ";
    m_pendingMissing = currentMissing;
    scheduleNackDebounce();
    qDebug() << "handling nack timeout";
}


void SlothRxSocket::scheduleNackDebounce() {
    if (m_nackDebounceScheduled) return;

    m_nackDebounceScheduled = true;
    QTimer::singleShot(100, this, &SlothRxSocket::performNackDebounce);
}


void SlothRxSocket::performNackDebounce() {
    QSet<quint32> stillMissing;

    for (quint32 seq : m_pendingMissing) {
        if (!m_receivedSeqNums.contains(seq)) {
            stillMissing.insert(seq);
        }
    }

    if (!stillMissing.isEmpty()) {
        qDebug() << "Sending debounced NACK for: " << stillMissing;
        sendNack(stillMissing.toList());
    }

    m_nackDebounceScheduled = false;
}


void SlothRxSocket::sendNack(QList<quint32> missing)
{
    if (missing.isEmpty()) return;

    QSet<quint32> missingSet = QSet<quint32>::fromList(missing);

    quint32 base = *std::min_element(missing.begin(), missing.end());
    int windowSize = m_windowSize;

    QByteArray bitmap = SlothPacketUtils::generateBitmapFromSet(base, windowSize, missingSet);

    NackPacket packet;
    packet.header.type = PacketType::NACK;
    packet.header.sequenceNumber = 1;

    packet.baseSeqNum = base;
    packet.bitmapLength = bitmap.size();
    packet.bitmap = bitmap;
    packet.header.payloadSize = bitmap.size() + sizeof(quint8) + sizeof(quint32);


    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);

    stream << packet.baseSeqNum
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

    transmitBuffer(full);
}


void SlothRxSocket::sayByeToPeer()
{
    PacketHeader header(PacketType::BYE, 0/*send session id here*/, 0);
    header.checksum = 0;
    transmitBuffer(header.serialize());
}
