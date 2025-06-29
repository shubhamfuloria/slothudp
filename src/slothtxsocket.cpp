#include "include/slothtxsocket.h"
#include "include/slothpacketutils.h"

#include <QRandomGenerator>
#include <QIODevice>
#include <QDataStream>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QElapsedTimer>

SlothTxSocket::SlothTxSocket()
{

    bool success = bind(4000);

    if(!success) {
        qWarning() << "Could not bind UDP Socket at port 4000 for SlothTx, file transfer will not work";
    }

    connect(this, &QUdpSocket::readyRead, this, &SlothTxSocket::handleReadyRead);
}


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
    m_destAddress = QHostAddress(destination);
    m_destPort = port;

    // generate handshake packet
    HandshakePacket packet;
    packet.filename = QFileInfo(filePath).fileName();
    packet.totalSize = fileSize;
    packet.requestId = QRandomGenerator::global()->generate();
    packet.protocolVersion = m_protoVer;

    qDebug() << "SlothTX:: packet ===>";
    // packet.print();

    QByteArray buffer = SlothPacketUtils::serializePacket(packet);

    // store session id, and mark session as not active
    m_activeSessionId = packet.requestId;
    m_sessionState = SessionState::REQPENDING;


    // send the buffer over to network
    transmitBuffer(buffer);


    // retry handshake packet, to handle the case of handshake packet loss
    m_handshakeRetryTimer = new QTimer(this);
    m_handshakeReqRetryCount = 0;
    connect(m_handshakeRetryTimer, &QTimer::timeout, this, [=]() {
        if (++m_handshakeReqRetryCount >= m_handshakeReqRetryLimit) {
            qWarning() << "Handshake retry limit reached. Giving up.";
            m_handshakeRetryTimer->stop();
            return;
        }

        qDebug() << "Retrying handshake... attempt" << m_handshakeReqRetryCount;
        transmitBuffer(buffer);
    });

    m_handshakeRetryTimer->start(500);

    m_rttTimer = new QElapsedTimer();
    m_rttTimer->start();

    m_retransmitTimer = new QTimer();
    connect(m_retransmitTimer, &QTimer::timeout, this, &SlothTxSocket::handleRetransmissions);
    m_retransmitTimer->start(300);
}


bool SlothTxSocket::transmitBuffer(const QByteArray& buffer)
{
    if(m_destAddress.isNull()) {
        qWarning() << "Destination address not set, cannot transmit buffer";
        return false;
    }

    QNetworkDatagram datagram;

    datagram.setData(buffer);
    datagram.setDestination(m_destAddress, m_destPort);
    quint64 bytesSent = writeDatagram(datagram);

    // -1 : failed to write datagram
    return bytesSent != -1;
}

void SlothTxSocket::handleReadyRead()
{
    while(hasPendingDatagrams()) {
        qDebug() << "TX received";
        QNetworkDatagram datagram = receiveDatagram(4096);
        // process the datagram
        QByteArray buffer = datagram.data();
        QByteArray payload;
        PacketHeader header;
        bool success = SlothPacketUtils::parsePacketHeader(buffer, header, payload);

        if(!success) {
            qWarning() << "SlothTX: Checksum failed, dropping packet";
            continue;
        }

        switch(header.type) {


        case PacketType::HANDSHAKEACK:
            qDebug() << "Received handshake acknowledgement";
            // at this point we should start sending file packets
            handleHandshakeAck(header.sequenceNumber);
            break;

        case PacketType::ACK:
            handleDataAck(header, payload);
            break;

        case PacketType::NACK:
            handleNack(header, payload);
            break;

        case PacketType::BYE:
            // received has successfully received EOF packet and we should now close the connection
            // before that make sure that we've received acknowledgment of all packets
            handleBye();
            break;

        default:
            qDebug() << "SlothTx:: received unexpected packet, dropping...";
        }
    }
}

void SlothTxSocket::handleHandshakeAck(quint32 requestId)
{

    if(requestId != m_activeSessionId) {
        qDebug() << "handshake acknowledgement from invalid session id, dropping...";
        return;
    }
    qDebug() << "Received handshake ack for request Id " << requestId;

    // handshake retry timer may still be running now
    if (m_handshakeRetryTimer) {
        qDebug() << "Stopping handshake retry timer";
        m_handshakeRetryTimer->stop();
        m_handshakeRetryTimer->deleteLater();
        m_handshakeRetryTimer = nullptr;
    }

    initiateFileTransfer();
}

// void SlothTxSocket::handleDataAck(PacketHeader header, QByteArray buffer)
// {
//     AckWindowPacket packet;
//     SlothPacketUtils::deserializePacket(buffer, packet);
//     quint32 base = packet.baseSeqNum;
//     QByteArray bitmap = packet.bitmap;

//     qDebug() << "SlothTX:: <=== ACK with base " << base;
//     SlothPacketUtils::logBitMap(bitmap);

//     // update m_baseSeq

//     // qDebug() << "m_sendWindow before ack calculation " << m_sendWindow.keys();
//     quint32 baseMinusOne = base > 0 ? base - 1 : base;
//      quint32 newBase = m_baseSeqNum > baseMinusOne ? m_baseSeqNum : baseMinusOne;
//     qDebug() << "updating m_base from " << m_baseSeqNum << " to " << newBase;

//     for(quint32 i = m_baseSeqNum; i <= newBase; i++) {
//         if(m_sendWindow.contains(i)) {

//             if(m_sentTimestamp.contains(i)) {
//                 quint64 sentTime = m_sentTimestamp.value(i);
//                 quint64 now = m_rttTimer->elapsed();

//                 quint64 rtt = now - sentTime;
//                 m_estimatedRTT = 0.875 * m_estimatedRTT + 0.125 * rtt;
//                 quint64 deviation = std::abs((qint64)rtt - (qint64)m_estimatedRTT);
//                 m_devRTT = 0.75 * m_devRTT + 0.25 * deviation;
//                 m_sentTimestamp.remove(i);
//         }
//             m_sendWindow.remove(i);
//         } }
//     m_baseSeqNum = newBase;

//     for(int i = 0; i < bitmap.size(); i++) {
//         quint8 byte = static_cast<quint8>(bitmap[i]);
//         for(int bit = 0; bit < 8; bit++) {
//             quint32 seq = base + i * 8 + bit;
//             bool isAcked = byte & (1 << (7 - bit));
//             if(isAcked) {

//                 if(m_sentTimestamp.contains(seq)) {
//                     quint64 sentTime = m_sentTimestamp.value(seq);
//                     quint64 now = m_rttTimer->elapsed();

//                     quint64 rtt = now - sentTime;
//                     m_estimatedRTT = 0.875 * m_estimatedRTT + 0.125 * rtt;
//                     quint64 deviation = std::abs((qint64)rtt - (qint64)m_estimatedRTT);
//                     m_devRTT = 0.75 * m_devRTT + 0.25 * deviation;
//                     m_sentTimestamp.remove(seq);
//                 }
//                 m_sendWindow.remove(seq);

//             }

//             // sender may send us early ack , like we send 16 packets to receiver
//             // receiver after receiving 8 packets may send ack and the ack may look like base: 8, 00000000
//             // which represents, we've received packet till 8, and 8 packets after 8 are missing
//             // though sender is still sending these packets
//             // so it's not viable to mark these 0s as missing packets

//             // we'll only mark missing packets when we receive an nack or a timeout hits in sender window
//             // otherwise we'll assume the packet has been delivered


//             // I think we shouldn't assume that each hole in bitmap is lost
//             // because we're getting ack in this form too 10000000 (base: 50), 50th pac
//             else if(seq < m_nextSeqNum){ // if there is a hole in ack, we mark this as missing packet, and send this packet in next transmission
//                 // m_missingWindow.insert(seq);
//             }
//         }
//     }
//     qDebug() << "m_estimated RTT " << m_estimatedRTT;
//     while (!m_sendWindow.contains(m_baseSeqNum) && m_baseSeqNum < m_nextSeqNum) {
//         ++m_baseSeqNum;
//     }
//     // qDebug() << "m_sendWindow after ack calculation " << m_sendWindow.keys();
//     sendNextWindow();
// }

void SlothTxSocket::handleNack(PacketHeader header, QByteArray buffer)
{
    NackPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);

    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;
    qDebug() << "SlothTx <=== NACK";
    SlothPacketUtils::logBitMap(bitmap);
    for (int i = 0; i < bitmap.size(); ++i) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i * 8 + bit;
            if (byte & (1 << (7 - bit))) {
                m_missingWindow.insert(seq);
            }
        }
    }
    sendNextWindow();
}


bool SlothTxSocket::initiateFileTransfer()
{
    m_file.setFileName(m_filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open file for reading:" << m_file.errorString();
        return false;
    }

    m_nextSeqNum = 0;
    m_baseSeqNum = 0;
    m_windowSize = 20;

    sendNextWindow();
    return true;
}

void SlothTxSocket::sendNextWindow()
{
    qDebug() << "Sending next window";
    if (!m_file.isOpen() || !m_file.isReadable()) {
        qWarning() << "File not open for reading!";
        return;
    }

    quint32 effectiveWindow = getEffectiveWindowSize();
    int packetsSent = 0;
    quint64 now = m_rttTimer->elapsed();

    qDebug() << QString("Effective window: %1 (CWnd: %2, Outstanding: %3)")
                    .arg(effectiveWindow).arg(m_congestionWindow).arg(m_sendWindow.size());

    // Prioritize retransmissions
    QList<quint32> missingList = m_missingWindow.toList();
    qSort(missingList);

    for (quint32 seq : missingList) {
        if (packetsSent >= (int)effectiveWindow) break;

        if (m_sendWindow.contains(seq)) {
            if (m_lastRextTime.contains(seq)) {
                quint64 lastSent = m_lastRextTime[seq];
                // Use adaptive spacing based on RTT
                quint64 minSpacing = qMax(m_RTO / 8, (quint64)50);
                if (now - lastSent < minSpacing) {
                    continue;
                }
            }

            QByteArray buffer = m_sendWindow[seq];
            qDebug() << "SlothTX: RETX seq " << seq << " ====> ";

            m_lastRextTime[seq] = now;
            transmitBuffer(buffer);
            packetsSent++;
        }
    }

    // Clear processed retransmissions
    for (quint32 seq : missingList) {
        if (m_lastRextTime.contains(seq) && m_lastRextTime[seq] == now) {
            m_missingWindow.remove(seq);
        }
    }

    // Send new packets within adaptive window
    while ((m_nextSeqNum < m_baseSeqNum + effectiveWindow) &&
           packetsSent < (int)effectiveWindow &&
           !m_file.atEnd()) {

        QByteArray chunk = m_file.read(m_chunkSize);
        if (chunk.isEmpty()) break;

        DataPacket packet;
        packet.header = PacketHeader(PacketType::DATA, m_nextSeqNum, chunk.size());
        packet.header.checksum = qChecksum(chunk.constData(), packet.header.payloadSize);
        packet.chunk = chunk;

        m_sentTimestamp[m_nextSeqNum] = now;
        QByteArray buffer = SlothPacketUtils::serializePacket(packet);

        qDebug() << "SlothTX: DATA seq " << packet.header.sequenceNumber << " ====> ";

        transmitBuffer(buffer);
        m_sendWindow[m_nextSeqNum] = buffer;
        ++m_nextSeqNum;
        ++packetsSent;
    }

    // Handle end of file
    if (m_file.atEnd() && m_sendWindow.empty()) {
        qDebug() << "All packets acknowledged. Sending FIN.";
        sendEOFPacket();
    }

    qDebug() << QString("Sent %1 packets. Window: [%2, %3), CWnd: %4, Outstanding: %5")
                    .arg(packetsSent).arg(m_baseSeqNum).arg(m_nextSeqNum)
                    .arg(m_congestionWindow).arg(m_sendWindow.size());
}


void SlothTxSocket::handleRetransmissions()
{
    quint64 now = m_rttTimer->elapsed();
    QList<quint32> timedOutPackets;

    for (auto it = m_sentTimestamp.begin(); it != m_sentTimestamp.end(); ++it) {
        quint32 seq = it.key();
        quint64 sentTime = it.value();

        if (now - sentTime >= m_RTO) {
            timedOutPackets.append(seq);
        }
    }

    if (!timedOutPackets.isEmpty()) {
        qDebug() << "Timeout detected for" << timedOutPackets.size() << "packets";

        // Timeout indicates severe congestion
        handleLossEvent(now);

        // Mark timed out packets for retransmission
        for (quint32 seq : timedOutPackets) {
            m_missingWindow.insert(seq);
            m_sentTimestamp.remove(seq);
        }

        // Exponential backoff
        m_RTO = qMin(m_RTO * 2, (quint64)3000);

        sendNextWindow();
    }
}

// void SlothTxSocket::sendPacket(DataPacket& packet)
// {

//     QByteArray buffer = SlothPacketUtils::serializePacket(packet);

//     transmitBuffer(buffer);
// }

quint32 SlothTxSocket::getEffectiveWindowSize()
{
    // Use minimum of congestion window and configured max window
    quint32 effectiveWindow = qMin(m_congestionWindow, m_maxWindow);
    effectiveWindow = qMax(effectiveWindow, m_minWindow);

    // Consider bandwidth-delay product for optimal window sizing
    if (m_estimatedBandwidth > 0 && m_estimatedRTT > 0) {
        // BDP = bandwidth * RTT (converted to packets)
        quint64 bdpBytes = (m_estimatedBandwidth * m_estimatedRTT);
        quint32 bdpPackets = (bdpBytes / m_chunkSize) + 1;

        // Use BDP as a guide, but don't exceed congestion window
        effectiveWindow = qMin(effectiveWindow, bdpPackets * 2); // 2x BDP for buffer
    }

    return effectiveWindow;
}

void SlothTxSocket::handleDataAck(PacketHeader header, QByteArray buffer)
{
    AckWindowPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);
    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

    qDebug() << "SlothTX:: <=== ACK with base " << base << "CWnd:" << m_congestionWindow;
    SlothPacketUtils::logBitMap(bitmap);

    bool lossDetected = false;
    quint32 newlyAckedPackets = 0;
    quint64 now = m_rttTimer->elapsed();

    // Process cumulative ACK
    if (base > m_baseSeqNum) {
        for (quint32 i = m_baseSeqNum; i < base; i++) {
            if (m_sendWindow.contains(i)) {
                updateRTTAndBandwidth(i, now);
                m_sendWindow.remove(i);
                m_missingWindow.remove(i);
                newlyAckedPackets++;
                m_bytesAcked += m_chunkSize;
            }
        }
        m_baseSeqNum = base;
    }

    // Process selective ACKs from bitmap
    for (int i = 0; i < bitmap.size(); i++) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for (int bit = 0; bit < 8; bit++) {
            quint32 seq = base + i * 8 + bit;
            bool isAcked = byte & (1 << (7 - bit));

            if (isAcked && m_sendWindow.contains(seq)) {
                updateRTTAndBandwidth(seq, now);
                m_sendWindow.remove(seq);
                m_missingWindow.remove(seq);
                newlyAckedPackets++;
                m_bytesAcked += m_chunkSize;
            } else if (!isAcked && seq < m_nextSeqNum && seq >= base) {
                // Hole in bitmap indicates loss
                if (m_sendWindow.contains(seq)) {
                    lossDetected = true;
                    m_missingWindow.insert(seq);
                }
            }
        }
    }

    // Detect duplicate ACKs
    if (base == m_lastAckSeq) {
        m_duplicateAckCount[base]++;
        if (m_duplicateAckCount[base] >= 3) {
            // Fast retransmit triggered
            lossDetected = true;
            qDebug() << "Fast retransmit triggered for base:" << base;

            // Mark next expected packet as lost
            quint32 nextExpected = base;
            if (m_sendWindow.contains(nextExpected)) {
                m_missingWindow.insert(nextExpected);
            }

            // Reset duplicate count
            m_duplicateAckCount[base] = 0;
        }
    } else {
        m_duplicateAckCount.clear(); // Reset on new ACK
        m_lastAckSeq = base;
    }

    // Update congestion window based on loss detection
    if (lossDetected) {
        handleLossEvent(now);
    } else if (newlyAckedPackets > 0) {
        handleSuccessfulAck(newlyAckedPackets, now);
    }

    // Update bandwidth estimation
    updateBandwidthEstimate(now);

    sendNextWindow();
}

void SlothTxSocket::handleLossEvent(quint64 now)
{
    qDebug() << "Loss detected! CWnd:" << m_congestionWindow << "-> ";

    // Multiplicative decrease
    m_slowStartThreshold = qMax(m_congestionWindow / 2, m_minWindow);

    // Different recovery strategies based on loss frequency
    quint64 timeSinceLastLoss = (m_lastLossTime > 0) ? (now - m_lastLossTime) : UINT64_MAX;

    if (timeSinceLastLoss < m_estimatedRTT * 4) {
        // Frequent losses - be more conservative
        m_congestionWindow = m_minWindow;
        m_recentLossCount++;
        qDebug() << "Frequent loss detected, aggressive backoff";
    } else {
        // Isolated loss - moderate backoff
        m_congestionWindow = qMax(m_congestionWindow / 2, m_minWindow);
        m_recentLossCount = 1;
    }

    m_consecutiveGoodAcks = 0;
    m_lastLossTime = now;

    qDebug() << m_congestionWindow << "SSThresh:" << m_slowStartThreshold;
}

void SlothTxSocket::updateRTTAndBandwidth(quint32 seq, quint64 now)
{
    if (m_sentTimestamp.contains(seq)) {
        quint64 sentTime = m_sentTimestamp.value(seq);
        quint64 rtt = now - sentTime;

        // Update RTT statistics
        m_rttSamples.enqueue(rtt);
        if (m_rttSamples.size() > 10) {
            m_rttSamples.dequeue(); // Keep only recent samples
        }

        m_minRTT = qMin(m_minRTT, rtt);

        // Smooth RTT calculation
        if (m_estimatedRTT == 0) {
            m_estimatedRTT = rtt;
            m_devRTT = rtt / 2;
        } else {
            m_estimatedRTT = 0.875 * m_estimatedRTT + 0.125 * rtt;
            quint64 deviation = std::abs((qint64)rtt - (qint64)m_estimatedRTT);
            m_devRTT = 0.75 * m_devRTT + 0.25 * deviation;
        }

        // Update RTO
        m_RTO = m_estimatedRTT + 4 * m_devRTT;
        m_RTO = qMax(m_RTO, (quint64)100);
        m_RTO = qMin(m_RTO, (quint64)3000);

        m_sentTimestamp.remove(seq);
    }
}



void SlothTxSocket::updateBandwidthEstimate(quint64 now)
{
    if (m_lastBandwidthCalc == 0) {
        m_lastBandwidthCalc = now;
        return;
    }

    quint64 timeDiff = now - m_lastBandwidthCalc;
    if (timeDiff >= 1000) { // Update every second
        if (m_bytesAcked > 0) {
            double currentBandwidth = (double)m_bytesAcked / timeDiff; // bytes/ms

            if (m_estimatedBandwidth == 0) {
                m_estimatedBandwidth = currentBandwidth;
            } else {
                // Smooth bandwidth estimate
                m_estimatedBandwidth = 0.8 * m_estimatedBandwidth + 0.2 * currentBandwidth;
            }

            qDebug() << "Bandwidth estimate:" << (m_estimatedBandwidth * 8 / 1000) << "Mbps";
        }

        m_bytesAcked = 0;
        m_lastBandwidthCalc = now;
    }
}

void SlothTxSocket::handleSuccessfulAck(quint32 newlyAckedPackets, quint64 now)
{
    m_consecutiveGoodAcks += newlyAckedPackets;

    // Adaptive window increase strategy
    if (m_congestionWindow < m_slowStartThreshold) {
        // Slow Start: exponential growth
        m_congestionWindow += newlyAckedPackets;
        qDebug() << "Slow Start: CWnd increased to" << m_congestionWindow;
    } else {
        // Congestion Avoidance: linear growth
        // Increase by 1 packet per RTT (approximated)
        if (m_consecutiveGoodAcks >= m_congestionWindow) {
            m_congestionWindow++;
            m_consecutiveGoodAcks = 0;
            qDebug() << "Congestion Avoidance: CWnd increased to" << m_congestionWindow;
        }
    }

    // Cap the window size
    m_congestionWindow = qMin(m_congestionWindow, m_maxWindow);

    // Adaptive maximum window based on performance
    if (now - m_lastWindowAdjustment > m_estimatedRTT * 4) {
        adaptMaxWindow();
        m_lastWindowAdjustment = now;
    }
}

void SlothTxSocket::adaptMaxWindow()
{
    // Adjust maximum window based on recent performance
    if (m_recentLossCount == 0 && m_consecutiveGoodAcks > m_maxWindow) {
        // No recent losses and good performance - increase max window
        m_maxWindow = qMin(m_maxWindow + 10, (quint32)200);
        qDebug() << "Increased max window to" << m_maxWindow;
    } else if (m_recentLossCount > 3) {
        // Frequent losses - decrease max window
        m_maxWindow = qMax(m_maxWindow - 5, m_minWindow * 2);
        qDebug() << "Decreased max window to" << m_maxWindow;
    }

    // Decay recent loss count
    m_recentLossCount = m_recentLossCount / 2;
}

void SlothTxSocket::sendEOFPacket()
{
    PacketHeader header(PacketType::FIN, m_activeSessionId/*reqId*/, 0);
    QByteArray buffer = header.serialize(0);

    transmitBuffer(buffer);
}

void SlothTxSocket::handleBye()
{
    // received has successfully
}

