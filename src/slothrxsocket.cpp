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

            // qDebug() << "SlothRx:: received data packet";
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

    // acknowledgeTxRequest(packet.requestId);


    QMetaObject::invokeMethod(this, [=]() {
                acknowledgeTxRequest(packet.requestId);
            }, Qt::QueuedConnection);
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

    startFeedbackTimers();
    return transmitBuffer(buffer);
}


void SlothRxSocket::handlePacket(PacketHeader header, QByteArray payload) {

    if(header.sequenceNumber < m_baseWriteSeqNum) {
        qDebug() << "Already written packet " << header.sequenceNumber;
        // Still send ACK for old packets to help sender advance
        if (m_untrackedCount == 0) {
            m_untrackedCount = 8; // Force ACK send
        }
        return;
    }

    m_highestSeqReceived = qMax(m_highestSeqReceived, header.sequenceNumber);
    m_recvWindow[header.sequenceNumber] = payload;
    m_receivedSeqNums.insert(header.sequenceNumber);

    qDebug() << "SlothRX <=== DATA seq " << header.sequenceNumber << ", untrackedCount: " << m_untrackedCount;

    // Write continuous packets to file
    while(m_recvWindow.contains(m_baseWriteSeqNum)) {
        QByteArray chunk = m_recvWindow[m_baseWriteSeqNum];
        m_file.write(chunk);
        m_recvWindow.remove(m_baseWriteSeqNum);
        m_baseWriteSeqNum++;
    }

    // Update cumulative ACK base
    while (m_receivedSeqNums.contains(m_baseAckSeqNum)) {
        ++m_baseAckSeqNum;
    }

    m_untrackedCount++;

    // IMPROVEMENT 2: More frequent ACKs and immediate ACK for gaps
    bool hasGap = (header.sequenceNumber > m_baseAckSeqNum);

    if (m_untrackedCount >= 4 || hasGap) { // Reduced from 8 to 4 for faster feedback
        qDebug() << "Sending acknowledgement (gap detected: " << hasGap << ")";
        sendAcknowledgement();
        m_untrackedCount = 0;
    }
}

void SlothRxSocket::sendAcknowledgement()
{
    // Use larger bitmap for better feedback (16 bytes = 128 bits)
    int bitmapSize = 16;
    QByteArray bitmap = generateAckBitmap(m_baseAckSeqNum, bitmapSize * 8);

    AckWindowPacket packet;
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);

    packet.baseSeqNum = m_baseAckSeqNum;
    packet.bitmapLength = bitmap.size();
    packet.bitmap = bitmap;

    stream << packet.baseSeqNum
           << static_cast<quint8>(bitmap.size());
    stream.writeRawData(bitmap.constData(), bitmap.size());

    packet.header = PacketHeader(PacketType::ACK, 1, payload.size());
    packet.header.checksum = qChecksum(payload.constData(), payload.size());

    QByteArray fullBuffer;
    QDataStream fullStream(&fullBuffer, QIODevice::WriteOnly);

    fullStream << static_cast<quint8>(packet.header.type)
               << packet.header.sequenceNumber
               << packet.header.payloadSize;

    packet.header.headerChecksum = qChecksum(fullBuffer.constData(), 7);
    fullStream << packet.header.headerChecksum
               << packet.header.checksum;

    fullStream.writeRawData(payload.constData(), payload.size());

    qDebug() << "SlothRX ACK with base " << m_baseAckSeqNum << " ===> ";
    SlothPacketUtils::logBitMap(bitmap);

    m_lastAckTime = QTime::currentTime();
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

    // Check for gaps in received sequence numbers
    for (quint32 i = m_baseAckSeqNum; i <= m_highestSeqReceived; ++i) {
        if (!m_receivedSeqNums.contains(i)) {
            currentMissing.insert(i);
        }
    }

    // Only send NACK if we have significant gaps and haven't sent one recently
    if (!currentMissing.isEmpty() && currentMissing.size() <= 20) { // Limit NACK size
        QTime now = QTime::currentTime();
        if (m_lastNackTime.msecsTo(now) >= 200) { // Don't spam NACKs
            m_pendingMissing = currentMissing;
            scheduleNackDebounce();
            m_lastNackTime = now;
        }
    }
}



void SlothRxSocket::scheduleNackDebounce() {
    if (m_nackDebounceScheduled) return;

    m_nackDebounceScheduled = true;
    QTimer::singleShot(100, this, &SlothRxSocket::performNackDebounce);
}


void SlothRxSocket::performNackDebounce() {
    QSet<quint32> stillMissing;

    for (quint32 seq : qAsConst(m_pendingMissing)) {
        if (!m_receivedSeqNums.contains(seq)) {
            stillMissing.insert(seq);
        }
    }

    if (!stillMissing.isEmpty()) {
        sendNack(stillMissing.toList());
    }

    m_nackDebounceScheduled = false;
}


void SlothRxSocket::sendNack(QList<quint32> missing)
{
    if (missing.isEmpty() || missing.size() > 50) return; // Don't send huge NACKs

    // Sort and limit the missing list
    qSort(missing);
    if (missing.size() > 20) {
        missing = missing.mid(0, 20); // Only NACK first 20 missing packets
    }

    QSet<quint32> missingSet = QSet<quint32>::fromList(missing);
    quint32 base = missing.first();
    quint32 range = missing.last() - base + 1;
    int windowSize = qMin((int)range, 64); // Limit bitmap size

    qDebug() << "Sending NACK for base " << base << ", count:" << missing.size();
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
           << static_cast<quint8>(bitmap.size());
    stream.writeRawData(bitmap.constData(), bitmap.size());

    packet.header.checksum = qChecksum(payload.constData(), payload.size());

    QByteArray full;
    QDataStream fullStream(&full, QIODevice::WriteOnly);
    fullStream << static_cast<quint8>(packet.header.type)
               << packet.header.sequenceNumber
               << packet.header.payloadSize;

    packet.header.headerChecksum = qChecksum(full.constData(), 7);
    fullStream << packet.header.headerChecksum
               << packet.header.checksum;
    fullStream.writeRawData(payload.constData(), payload.size());

    m_lastAckTime = QTime::currentTime();
    transmitBuffer(full);
}


void SlothRxSocket::handleFeedbackTimeout()
{
    QTime now = QTime::currentTime();
    int ms = m_lastAckTime.msecsTo(now);

    // Send ACK if no feedback sent recently (reduced from 200ms to 100ms)
    if (ms >= 100) {
        qDebug() << "Feedback timeout - sending ACK";
        sendAcknowledgement();
    }
}

void SlothRxSocket::startFeedbackTimers()
{
    m_nackTimer = new QTimer(this);
    m_feedbackTimer = new QTimer(this);

    connect(m_nackTimer, &QTimer::timeout, this, &SlothRxSocket::handleNackTimeout);
    connect(m_feedbackTimer, &QTimer::timeout, this, &SlothRxSocket::handleFeedbackTimeout);

    // More frequent timers for better responsiveness
    m_nackTimer->start(250);     // Reduced from 500ms
    m_feedbackTimer->start(50);  // Reduced from 500ms for quicker ACK sending

    m_lastAckTime = QTime::currentTime();
    m_lastNackTime = QTime::currentTime();
}


void SlothRxSocket::sayByeToPeer()
{
    PacketHeader header(PacketType::BYE, 0/*send session id here*/, 0);
    header.checksum = 0;
    transmitBuffer(header.serialize());
}
