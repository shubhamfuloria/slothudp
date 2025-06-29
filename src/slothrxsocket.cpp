#include "include/slothrxsocket.h"
#include "include/slothpacketutils.h"

#include <QNetworkDatagram>
#include <QIODevice>
#include <QDataStream>

// #define DEBUG_RX_SOCKET

SlothRxSocket::SlothRxSocket() {
    // Initialize stats
    m_stats.totalBytesReceived = 0;
    m_stats.uniqueBytesReceived = 0;
    m_stats.totalPacketsReceived = 0;
    m_stats.duplicatePacketsReceived = 0;
    m_stats.totalPacketsLost = 0;
    m_stats.outOfOrderPackets = 0;
    m_progressStartTime = QTime::currentTime();

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
        m_stats.totalBytesReceived += buffer.size();

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
            m_stats.totalPacketsReceived++;

#ifdef DEBUG_RX_SOCKET
            qDebug() << "SlothRx:: received data packet";
#endif
            handlePacket(header, payload);

            break;

        case PacketType::FIN:
#ifdef DEBUG_RX_SOCKET
            qDebug() << "Recieved fin packet, we're safe to close the file now";
#endif
            handleFinPacket(header);
            // // we should send sender a confirmation packet and tear down connection
    //         stopProgressTimer();
    //         printFinalStats();
    //         m_file.close();
            break;

        default:
#ifdef DEBUG_RX_SOCKET
            qDebug() << "SlothRx: Packet type not handled";
#endif
            break;
        }
    }
}

void SlothRxSocket::handleHandshakePacket(QByteArray buffer)
{
#ifdef DEBUG_RX_SOCKET
    qDebug() << "handling handshake packet";
#endif
    HandshakePacket packet = SlothPacketUtils::deserializePacket(buffer);

    /*
     * check if it's a duplicate handshake request, if so ignore it
     * if it's a new handshake request, then reject it
    */

    if (m_sessionState != SessionState::NOTACTIVE) {
        if (packet.requestId == m_activeSessionId) {
#ifdef DEBUG_RX_SOCKET
            qDebug() << "Duplicate handshake packet received. Ignoring.";
#endif
        } else {
#ifdef DEBUG_RX_SOCKET
            qDebug() << "Another session is already active. Rejecting new handshake.";
#endif
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
    m_expectedFileSize = packet.totalSize;

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
#ifdef DEBUG_RX_SOCKET
        qDebug() << "Could not open file " << m_filePath;
#endif
    } else {
#ifdef DEBUG_RX_SOCKET
        qDebug() << "Opened Successfully file " << m_filePath;
#endif
    }

    /*
     * Start NACK Timer
     *
    */

    startFeedbackTimers();
    startProgressTimer();

    return transmitBuffer(buffer);
}

void SlothRxSocket::handlePacket(PacketHeader header, QByteArray payload) {

    if(header.sequenceNumber < m_baseWriteSeqNum) {
        m_stats.duplicatePacketsReceived++;
        // For duplicates, only ACK if we haven't sent one recently
        if (m_packetsSinceLastAck > m_adaptiveAckThreshold * 2) {
            scheduleDelayedAck();
        }
        return;
    }

    // Check if this is an out-of-order packet
    if (header.sequenceNumber > m_baseWriteSeqNum) {
        m_stats.outOfOrderPackets++;
    }

    m_highestSeqReceived = qMax(m_highestSeqReceived, header.sequenceNumber);
    m_recvWindow[header.sequenceNumber] = payload;
    m_receivedSeqNums.insert(header.sequenceNumber);

    // Bandwidth estimation
    m_consecutivePackets++;
    m_bwMeasureBytes += payload.size();
    m_packetsSinceLastAck++;

    // Write continuous packets to file
    while(m_recvWindow.contains(m_baseWriteSeqNum)) {
        QByteArray chunk = m_recvWindow[m_baseWriteSeqNum];
        m_file.write(chunk);
        m_stats.uniqueBytesReceived += chunk.size();
        m_recvWindow.remove(m_baseWriteSeqNum);
        m_baseWriteSeqNum++;
    }

    // Update cumulative ACK base
    while (m_receivedSeqNums.contains(m_baseAckSeqNum)) {
        ++m_baseAckSeqNum;
    }

    m_untrackedCount++;

    // OPTIMIZED ACK STRATEGY
    bool hasSignificantGap = isSignificantGap(header.sequenceNumber);
    bool shouldAckNow = false;

    if (hasSignificantGap) {
        // Immediate ACK for significant gaps
        shouldAckNow = true;
        m_lastSignificantGap = QTime::currentTime();
    } else if (m_isLowBandwidth) {
        // Low bandwidth: Very conservative ACKing
        shouldAckNow = (m_untrackedCount >= m_adaptiveAckThreshold * 3);
    } else {
        // Normal bandwidth: Standard threshold
        shouldAckNow = (m_untrackedCount >= m_adaptiveAckThreshold);
    }

    if (shouldAckNow) {
        sendAcknowledgement();
        m_untrackedCount = 0;
        m_packetsSinceLastAck = 0;
    } else if (!m_pendingAck && m_untrackedCount >= m_adaptiveAckThreshold / 2) {
        // Schedule delayed ACK for efficiency
        scheduleDelayedAck();
    }

    // Update parameters less frequently to reduce overhead
    if (m_consecutivePackets % 50 == 0) {
        estimateBandwidth();
        updateAdaptiveParameters();
        optimizeForBandwidth();
    }
}

void SlothRxSocket::scheduleDelayedAck() {
    if (m_pendingAck) return; // Already scheduled

    m_pendingAck = true;

    if (!m_ackBatchTimer) {
        m_ackBatchTimer = new QTimer(this);
        m_ackBatchTimer->setSingleShot(true);
        connect(m_ackBatchTimer, &QTimer::timeout, this, [this]() {
            if (m_pendingAck) {
                sendAcknowledgement();
                m_untrackedCount = 0;
                m_packetsSinceLastAck = 0;
                m_pendingAck = false;
            }
        });
    }

    // Delay ACK by adaptive interval
    quint32 delayMs = m_isLowBandwidth ? 300 : 100;
    m_ackBatchTimer->start(delayMs);
}

bool SlothRxSocket::isSignificantGap(quint32 seqNum) {
    quint32 gap = seqNum - m_baseAckSeqNum;

    // Dynamic gap threshold based on bandwidth
    if (m_isLowBandwidth) {
        return gap > m_gapThreshold * 2; // More tolerant at low BW
    } else {
        return gap > m_gapThreshold;
    }
}



void SlothRxSocket::estimateBandwidth() {
    if (m_bwMeasureStart.isNull()) {
        m_bwMeasureStart = QTime::currentTime();
        return;
    }

    int elapsedMs = m_bwMeasureStart.msecsTo(QTime::currentTime());
    if (elapsedMs > 1000) { // Measure over 1 second windows
        quint32 measuredBw = (m_bwMeasureBytes * 1000) / elapsedMs; // bytes/sec

        // Smooth the estimate (exponential moving average)
        m_estimatedBandwidth = (m_estimatedBandwidth * 7 + measuredBw) / 8;

        // Reset measurement window
        m_bwMeasureStart = QTime::currentTime();
        m_bwMeasureBytes = 0;

#ifdef DEBUG_RX_SOCKET
        qDebug() << "Estimated bandwidth:" << m_estimatedBandwidth << "bytes/sec";
#endif
    }
}

void SlothRxSocket::optimizeForBandwidth() {
    m_isLowBandwidth = (m_estimatedBandwidth < 15000); // < 15KB/s

    if (m_isLowBandwidth) {
        // Low bandwidth optimizations
        m_minAckInterval = 400; // Minimum 400ms between ACKs
        m_gapThreshold = 5;     // More tolerant of gaps

        // Reduce NACK frequency even more
        if (m_nackTimer) {
            m_nackTimer->setInterval(1500); // 1.5 seconds
        }
    } else {
        // Normal bandwidth settings
        m_minAckInterval = 100;
        m_gapThreshold = 3;

        if (m_nackTimer) {
            m_nackTimer->setInterval(qMax(300U, m_measuredRtt * 2));
        }
    }
}


void SlothRxSocket::updateAdaptiveParameters() {
    if (m_estimatedBandwidth < 5000) {
        // Very low bandwidth (< 5KB/s): Extreme conservation
        m_adaptiveAckThreshold = 25;
        m_adaptiveFeedbackInterval = 2000; // 2 seconds
    } else if (m_estimatedBandwidth < 15000) {
        // Low bandwidth (5-15 KB/s): Conservative
        m_adaptiveAckThreshold = 20;
        m_adaptiveFeedbackInterval = 1000;
    } else if (m_estimatedBandwidth < 50000) {
        // Medium-low bandwidth (15-50 KB/s): Moderate
        m_adaptiveAckThreshold = 12;
        m_adaptiveFeedbackInterval = 500;
    } else if (m_estimatedBandwidth < 200000) {
        // Medium bandwidth (50-200 KB/s): Standard
        m_adaptiveAckThreshold = 8;
        m_adaptiveFeedbackInterval = 200;
    } else {
        // High bandwidth (> 200KB/s): Aggressive
        m_adaptiveAckThreshold = 4;
        m_adaptiveFeedbackInterval = 100;
    }

    // RTT-based adjustments
    if (m_measuredRtt > 300) {
        m_adaptiveAckThreshold = qMax(4U, m_adaptiveAckThreshold / 2);
    }
}


void SlothRxSocket::sendAcknowledgement() {
    QTime now = QTime::currentTime();

    // Rate limit ACKs to respect minimum interval
    if (m_minAckInterval > 0 && m_lastAckTime.msecsTo(now) < (int)m_minAckInterval) {
        // Schedule delayed ACK instead of immediate
        scheduleDelayedAck();
        return;
    }

    // Adaptive bitmap size based on bandwidth
    int bitmapSize;
    if (m_isLowBandwidth) {
        bitmapSize = 8;  // 64 bits - smaller for low BW
    } else if (m_estimatedBandwidth < 100000) {
        bitmapSize = 12; // 96 bits - medium
    } else {
        bitmapSize = 16; // 128 bits - larger for high BW
    }

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

#ifdef DEBUG_RX_SOCKET
    qDebug() << "SlothRX ACK (rate-limited) with base " << m_baseAckSeqNum;
#endif

    m_lastAckTime = now;
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

void SlothRxSocket::handleNackTimeout() {
    // Skip NACK if we're in a silent period (just received packets)
    QTime now = QTime::currentTime();
    if (m_silentPeriodMs > 0 && m_lastSignificantGap.msecsTo(now) < (int)m_silentPeriodMs) {
        return;
    }

    QSet<quint32> currentMissing;
    for (quint32 i = m_baseAckSeqNum; i <= m_highestSeqReceived; ++i) {
        if (!m_receivedSeqNums.contains(i)) {
            currentMissing.insert(i);
        }
    }

    m_stats.totalPacketsLost = currentMissing.size();

    // More conservative NACK strategy
    quint32 maxNackSize = m_isLowBandwidth ? 10 : 30;
    quint32 minNackInterval = m_isLowBandwidth ? 2000 : 500;

    if (!currentMissing.isEmpty() &&
        currentMissing.size() <= maxNackSize &&
        currentMissing.size() >= 3) { // Only NACK if we have several missing

        if (m_lastNackTime.msecsTo(now) >= (int)minNackInterval) {
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

#ifdef DEBUG_RX_SOCKET
    qDebug() << "Sending NACK for base " << base << ", count:" << missing.size();
#endif
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

void SlothRxSocket::handleFeedbackTimeout() {
    QTime now = QTime::currentTime();
    int ms = m_lastAckTime.msecsTo(now);

    // Adaptive feedback timeout based on bandwidth
    quint32 timeoutMs = m_adaptiveFeedbackInterval;

    if (ms >= (int)timeoutMs) {
        sendAcknowledgement();
    }

    // Update timer interval if it changed
    if (m_feedbackTimer->interval() != (int)m_adaptiveFeedbackInterval) {
        m_feedbackTimer->setInterval(m_adaptiveFeedbackInterval);
    }
}

void SlothRxSocket::startFeedbackTimers() {
    m_nackTimer = new QTimer(this);
    m_feedbackTimer = new QTimer(this);

    connect(m_nackTimer, &QTimer::timeout, this, &SlothRxSocket::handleNackTimeout);
    connect(m_feedbackTimer, &QTimer::timeout, this, &SlothRxSocket::handleFeedbackTimeout);

    // Start with conservative timing, will adapt
    m_nackTimer->start(qMax(500U, m_measuredRtt * 3));
    m_feedbackTimer->start(m_adaptiveFeedbackInterval);

    m_lastAckTime = QTime::currentTime();
    m_lastNackTime = QTime::currentTime();

    // Initialize bandwidth measurement
    m_bwMeasureStart = QTime::currentTime();
}


void SlothRxSocket::startProgressTimer()
{
    m_progressTimer = new QTimer(this);
    connect(m_progressTimer, &QTimer::timeout, this, &SlothRxSocket::printProgress);
    m_progressTimer->start(1000); // Print progress every second
    m_progressStartTime = QTime::currentTime();
}

void SlothRxSocket::stopProgressTimer()
{
    if (m_progressTimer) {
        m_progressTimer->stop();
        delete m_progressTimer;
        m_progressTimer = nullptr;
    }
}

void SlothRxSocket::printProgress() {
    QTime now = QTime::currentTime();
    int elapsedMs = m_progressStartTime.msecsTo(now);

    if (elapsedMs == 0) return;

    double elapsedSec = elapsedMs / 1000.0;
    double progressPercent = (m_expectedFileSize > 0) ?
                                 (double(m_stats.uniqueBytesReceived) / m_expectedFileSize) * 100.0 : 0.0;

    double bytesPerSec = m_stats.uniqueBytesReceived / elapsedSec;
    double mbps = (bytesPerSec * 8) / (1024 * 1024);

    double efficiency = (m_stats.totalBytesReceived > 0) ?
                            (double(m_stats.uniqueBytesReceived) / m_stats.totalBytesReceived) * 100.0 : 0.0;

    qInfo() << "=== ADAPTIVE RX PROGRESS ===";
    qInfo() << QString("Progress: %1% (%2/%3 bytes)")
                   .arg(progressPercent, 0, 'f', 1)
                   .arg(m_stats.uniqueBytesReceived)
                   .arg(m_expectedFileSize);
    qInfo() << QString("Speed: %1 Mbps (%2 KB/s)")
                   .arg(mbps, 0, 'f', 2)
                   .arg(bytesPerSec / 1024, 0, 'f', 1);
    qInfo() << QString("Estimated BW: %1 KB/s, RTT: %2ms")
                   .arg(m_estimatedBandwidth / 1024)
                   .arg(m_measuredRtt);
    qInfo() << QString("Adaptive Params - ACK Threshold: %1, Feedback Interval: %2ms")
                   .arg(m_adaptiveAckThreshold)
                   .arg(m_adaptiveFeedbackInterval);
    qInfo() << QString("Efficiency: %1% Window: Base=%2, Buffered=%3")
                   .arg(efficiency, 0, 'f', 1)
                   .arg(m_baseWriteSeqNum)
                   .arg(m_recvWindow.size());
    qInfo() << "============================";
}

void SlothRxSocket::printFinalStats()
{
    QTime now = QTime::currentTime();
    int elapsedMs = m_progressStartTime.msecsTo(now);
    double elapsedSec = elapsedMs / 1000.0;

    double avgBytesPerSec = (elapsedSec > 0) ? m_stats.uniqueBytesReceived / elapsedSec : 0;
    double avgMbps = (avgBytesPerSec * 8) / (1024 * 1024);

    double efficiency = (m_stats.totalBytesReceived > 0) ?
                            (double(m_stats.uniqueBytesReceived) / m_stats.totalBytesReceived) * 100.0 : 0.0;

    qInfo() << "=== RX FINAL STATS ===";
    qInfo() << QString("Total Time: %1 seconds").arg(elapsedSec, 0, 'f', 2);
    qInfo() << QString("Average Speed: %1 Mbps (%2 KB/s)")
                   .arg(avgMbps, 0, 'f', 2)
                   .arg(avgBytesPerSec / 1024, 0, 'f', 1);
    qInfo() << QString("Final Efficiency: %1%").arg(efficiency, 0, 'f', 1);
    qInfo() << QString("Total Packets: %1, Duplicates: %2, Out-of-Order: %3")
                   .arg(m_stats.totalPacketsReceived)
                   .arg(m_stats.duplicatePacketsReceived)
                   .arg(m_stats.outOfOrderPackets);
    qInfo() << "=====================";
}

void SlothRxSocket::handleFinPacket(PacketHeader header)
{
    if (m_transferCompleted) {
        // Duplicate FIN - just resend BYE
        sendByeConfirmation();
        return;
    }

    // Verify all expected data was received
    bool allDataReceived = (m_stats.uniqueBytesReceived >= m_expectedFileSize);

    if (!allDataReceived) {
        qWarning() << QString("Transfer incomplete! Expected: %1, Received: %2")
                          .arg(m_expectedFileSize)
                          .arg(m_stats.uniqueBytesReceived);

        // Send final NACK for any remaining missing packets
        handleNackTimeout();

        // Set a timeout to force completion if missing packets aren't recovered
        QTimer::singleShot(2000, this, [=]() {
            qWarning() << "Forcing transfer completion despite missing data";
            performReceiveCleanup(false); // false = incomplete
        });
        return;
    }

    performReceiveCleanup(true); // true = successful
}


void SlothRxSocket::performReceiveCleanup(bool successful) {
    if (m_transferCompleted) return;

    m_transferCompleted = true;

    // Stop all timers
    stopProgressTimer();

    if (m_nackTimer) {
        m_nackTimer->stop();
        m_nackTimer->deleteLater();
        m_nackTimer = nullptr;
    }

    if (m_feedbackTimer) {
        m_feedbackTimer->stop();
        m_feedbackTimer->deleteLater();
        m_feedbackTimer = nullptr;
    }

    if (m_ackBatchTimer) {
        m_ackBatchTimer->stop();
        m_ackBatchTimer->deleteLater();
        m_ackBatchTimer = nullptr;
    }

    // Rest of cleanup code remains the same...
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
        qInfo() << "File closed successfully:" << m_filePath;
    }

    printFinalStats();

    if (successful) {
        qInfo() << "=== FILE RECEIVED SUCCESSFULLY ===";
    } else {
        qWarning() << "=== FILE TRANSFER INCOMPLETE ===";
    }

    sendByeConfirmation();

    m_recvWindow.clear();
    m_receivedSeqNums.clear();
    m_pendingMissing.clear();
    m_sessionState = SessionState::NOTACTIVE;
    m_activeSessionId = 0;

    emit transferCompleted(successful, m_filePath, m_stats.uniqueBytesReceived,
                           m_progressStartTime.msecsTo(QTime::currentTime()));

    qInfo() << "Receiver cleanup completed";
}

// Enhanced sendByeConfirmation method:
void SlothRxSocket::sendByeConfirmation()
{
    PacketHeader header(PacketType::BYE, m_activeSessionId, 0);
    QByteArray buffer = header.serialize(0);

    qInfo() << "Sending BYE confirmation to transmitter";
    transmitBuffer(buffer);

    // Send BYE multiple times to ensure delivery (since UDP is unreliable)
    m_byeConfirmTimer = new QTimer(this);
    int byeCount = 0;

    connect(m_byeConfirmTimer, &QTimer::timeout, this, [=]() mutable {
        if (++byeCount >= 3) {
            m_byeConfirmTimer->stop();
            m_byeConfirmTimer->deleteLater();
            return;
        }
        transmitBuffer(buffer);
    });

    m_byeConfirmTimer->start(500); // Send BYE 3 times with 500ms intervals
}


void SlothRxSocket::sayByeToPeer()
{
    PacketHeader header(PacketType::BYE, 0/*send session id here*/, 0);
    header.checksum = 0;
    transmitBuffer(header.serialize());
}
