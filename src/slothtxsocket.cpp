#include "include/slothtxsocket.h"
#include "include/slothpacketutils.h"

#include <QRandomGenerator>
#include <QIODevice>
#include <QDataStream>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QElapsedTimer>
#include <QDateTime>

// Debug control macros
// #define DEBUG_TX_SOCKET

SlothTxSocket::SlothTxSocket()
{
    bool success = bind(4000);

    if(!success) {
        qWarning() << "Could not bind UDP Socket at port 4000 for SlothTx, file transfer will not work";
    }

    connect(this, &QUdpSocket::readyRead, this, &SlothTxSocket::handleReadyRead);

    // Initialize progress tracking
    initializeProgressTracking();
}

void SlothTxSocket::initializeProgressTracking()
{
    // Initialize stats
    m_stats = {};
    m_stats.transferStartTime = QDateTime::currentMSecsSinceEpoch();

    // Setup progress timer
    m_progressTimer = new QTimer(this);
    connect(m_progressTimer, &QTimer::timeout, this, &SlothTxSocket::printTransmissionProgress);
    // Timer will be started when file transfer begins
}

void SlothTxSocket::printTransmissionProgress()
{
    quint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    quint64 elapsedMs = currentTime - m_stats.transferStartTime;

    if (elapsedMs == 0) return; // Avoid division by zero

    // Calculate current metrics
    double elapsedSec = elapsedMs / 1000.0;
    double bytesPerSec = m_stats.totalBytesSent / elapsedSec;
    double mbps = (bytesPerSec * 8) / (1024 * 1024); // Convert to Mbps

    // Calculate packet loss rate
    double lossRate = 0.0;
    if (m_stats.totalPacketsSent > 0) {
        lossRate = (double)m_stats.totalPacketsLost / m_stats.totalPacketsSent * 100.0;
    }

    // Calculate transmission efficiency
    double efficiency = 0.0;
    if (m_stats.totalBytesSent > 0) {
        efficiency = (double)m_stats.uniqueBytesSent / m_stats.totalBytesSent * 100.0;
    }

    // Calculate progress percentage
    double progressPercent = 0.0;
    qDebug() << "m_fileSize: " << m_fileSize;
    if (m_fileSize > 0) {
        // progressPercent = (double)m_stats.uniqueBytesSent / m_fileSize * 100.0;
        progressPercent = (double)m_stats.uniqueBytesSent / m_fileSize * 100.0;

    }

    qInfo() << "=== TX PROGRESS ===";
    qInfo() << QString("Progress: %1% (%2/%3 bytes)")
                   .arg(progressPercent, 0, 'f', 1)
                   .arg(m_stats.uniqueBytesSent)
                   .arg(m_fileSize);
    qInfo() << QString("Speed: %1 Mbps (%2 KB/s)")
                   .arg(mbps, 0, 'f', 2)
                   .arg(bytesPerSec / 1024, 0, 'f', 1);
    qInfo() << QString("Efficiency: %1% (Unique: %2, Total: %3)")
                   .arg(efficiency, 0, 'f', 1)
                   .arg(m_stats.uniqueBytesSent)
                   .arg(m_stats.totalBytesSent);
    qInfo() << QString("Packet Loss: %1% (%2/%3)")
                   .arg(lossRate, 0, 'f', 2)
                   .arg(m_stats.totalPacketsLost)
                   .arg(m_stats.totalPacketsSent);
    qInfo() << QString("RTT: %1ms, CWnd: %2, Outstanding: %3")
                   .arg(m_estimatedRTT)
                   .arg(m_congestionWindow)
                   .arg(m_sendWindow.size());
    qInfo() << QString("Retransmissions: %1, Timeouts: %2")
                   .arg(m_stats.totalRetransmissions)
                   .arg(m_stats.totalTimeouts);
    qInfo() << "==================";
}

void SlothTxSocket::initiateHandshake(
    const QString &filePath,
    qint64 fileSize,
    QString destination,
    quint16 port)
{
// #ifdef DEBUG_TX_SOCKET
    qDebug() << QString("Initiating handshake with %1:%2, sending file %3 of size %4")
                    .arg(destination)
                    .arg(port).arg(filePath)
                    .arg(fileSize);
// #endif

    m_filePath = filePath;
    m_fileSize = fileSize;
    m_destAddress = QHostAddress(destination);
    m_destPort = port;

    // Reset stats for new transfer
    m_stats = {};
    m_stats.transferStartTime = QDateTime::currentMSecsSinceEpoch();

    // generate handshake packet
    HandshakePacket packet;
    packet.filename = QFileInfo(filePath).fileName();
    packet.totalSize = fileSize;
    packet.requestId = QRandomGenerator::global()->generate();
    packet.protocolVersion = m_protoVer;

#ifdef DEBUG_TX_SOCKET
    qDebug() << "SlothTX:: packet ===>";
    // packet.print();
#endif

    QByteArray buffer = SlothPacketUtils::serializePacket(packet);

    // store session id, and mark session as not active
    m_activeSessionId = packet.requestId;
    m_sessionState = SessionState::REQPENDING;

    // send the buffer over to network
    transmitBuffer(buffer);
    m_stats.totalPacketsSent++;

    // retry handshake packet, to handle the case of handshake packet loss
    m_handshakeRetryTimer = new QTimer(this);
    m_handshakeReqRetryCount = 0;
    connect(m_handshakeRetryTimer, &QTimer::timeout, this, [=]() {
        if (++m_handshakeReqRetryCount >= m_handshakeReqRetryLimit) {
            qWarning() << "Handshake retry limit reached. Giving up.";
            m_handshakeRetryTimer->stop();
            return;
        }

// #ifdef DEBUG_TX_SOCKET
        qDebug() << "Retrying handshake... attempt" << m_handshakeReqRetryCount;
// #endif
        transmitBuffer(buffer);
        m_stats.totalPacketsSent++;
        m_stats.totalRetransmissions++;
    });

    m_handshakeRetryTimer->start(1000);

    m_rttTimer = new QElapsedTimer();
    m_rttTimer->start();

    m_retransmitTimer = new QTimer(this);
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

    // Update stats
    if (bytesSent != -1) {
        m_stats.totalBytesSent += buffer.size();
    }

    // -1 : failed to write datagram
    return bytesSent != -1;
}

void SlothTxSocket::handleReadyRead()
{
    while(hasPendingDatagrams()) {
#ifdef DEBUG_TX_SOCKET
        qDebug() << "TX received";
#endif
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
#ifdef DEBUG_TX_SOCKET
            qDebug() << "Received handshake acknowledgement";
#endif
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
#ifdef DEBUG_TX_SOCKET
            qDebug() << "SlothTx:: received unexpected packet, dropping...";
#endif
            break;
        }
    }
}

void SlothTxSocket::handleHandshakeAck(quint32 requestId)
{
    if(requestId != m_activeSessionId) {
#ifdef DEBUG_TX_SOCKET
        qDebug() << "handshake acknowledgement from invalid session id, dropping...";
#endif
        return;
    }

#ifdef DEBUG_TX_SOCKET
    qDebug() << "Received handshake ack for request Id " << requestId;
#endif

    // handshake retry timer may still be running now
    if (m_handshakeRetryTimer) {
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Stopping handshake retry timer";
#endif
        m_handshakeRetryTimer->stop();
        m_handshakeRetryTimer->deleteLater();
        m_handshakeRetryTimer = nullptr;
    }
    m_progressStartTime = QTime::currentTime();
    initiateFileTransfer();
}

void SlothTxSocket::handleNack(PacketHeader header, QByteArray buffer)
{
    NackPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);

    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

#ifdef DEBUG_TX_SOCKET
    qDebug() << "SlothTx <=== NACK";
    SlothPacketUtils::logBitMap(bitmap);
#endif

    for (int i = 0; i < bitmap.size(); ++i) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i * 8 + bit;
            if (byte & (1 << (7 - bit))) {
                if(!m_missingWindow.contains(seq)) {
                    m_missingWindow.insert(seq);
                    m_stats.totalPacketsLost++;
                }
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

    // Start progress monitoring
    m_progressTimer->start(1000); // Print progress every second

    sendNextWindow();
    return true;
}

void SlothTxSocket::sendNextWindow()
{
#ifdef DEBUG_TX_SOCKET
    qDebug() << "Sending next window";
#endif

    if (!m_file.isOpen() || !m_file.isReadable()) {
        qWarning() << "File not open for reading!";
        return;
    }

    quint32 effectiveWindow = getEffectiveWindowSize();
    int packetsSent = 0;
    quint64 now = m_rttTimer->elapsed();

#ifdef DEBUG_TX_SOCKET
    qDebug() << QString("Effective window: %1 (CWnd: %2, Outstanding: %3)")
                    .arg(effectiveWindow).arg(m_congestionWindow).arg(m_sendWindow.size());
#endif

    // Prioritize retransmissions
    QList<quint32> missingList = m_missingWindow.toList();
    qSort(missingList);

    for (quint32 seq : missingList) {
        if (packetsSent >= (int)effectiveWindow) break;

        if (m_sendWindow.contains(seq)) {
            if (m_lastRextTime.contains(seq)) {
                quint64 lastSent = m_lastRextTime[seq];
                // Use adaptive spacing based on RTT
                quint64 spacingByRTT = m_RTO / 2;
                quint64 spacingByLatency = m_estimatedRTT + m_RTO / 4;
                // quint64 minSpacing = qMax(spacingByRTT, spacingByLatency);
                quint64 minSpacing = m_RTO / 2;
                if (now - lastSent < minSpacing) {
                    continue;
                }
            }

            QByteArray buffer = m_sendWindow[seq];
#ifdef DEBUG_TX_SOCKET
            qDebug() << "SlothTX: RETX seq " << seq << " ====> ";
#endif

            m_lastRextTime[seq] = now;
            transmitBuffer(buffer);
            packetsSent++;
            m_stats.totalPacketsSent++;
            m_stats.totalRetransmissions++;
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

#ifdef DEBUG_TX_SOCKET
        qDebug() << "SlothTX: DATA seq " << packet.header.sequenceNumber << " ====> ";
#endif

        transmitBuffer(buffer);
        m_sendWindow[m_nextSeqNum] = buffer;
        ++m_nextSeqNum;
        ++packetsSent;
        m_stats.totalPacketsSent++;
        m_stats.uniqueBytesSent += chunk.size();
    }

    // Handle end of file
    if (m_file.atEnd() && m_sendWindow.empty()) {
#ifdef DEBUG_TX_SOCKET
        qDebug() << "All packets acknowledged. Sending FIN.";
#endif
        sendEOFPacket();

        // Stop progress timer when transfer is complete
        if (m_progressTimer) {
            m_progressTimer->stop();
            printTransmissionProgress(); // Final progress report
        }
    }

#ifdef DEBUG_TX_SOCKET
    qDebug() << QString("Sent %1 packets. Window: [%2, %3), CWnd: %4, Outstanding: %5")
                    .arg(packetsSent).arg(m_baseSeqNum).arg(m_nextSeqNum)
                    .arg(m_congestionWindow).arg(m_sendWindow.size());
#endif
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
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Timeout detected for" << timedOutPackets.size() << "packets";
#endif

        // Timeout indicates severe congestion
        handleLossEvent(now);

        // Mark timed out packets for retransmission
        for (quint32 seq : timedOutPackets) {
            m_missingWindow.insert(seq);
            m_sentTimestamp.remove(seq);
            m_stats.totalPacketsLost++;
            m_stats.totalTimeouts++;
        }

        // Exponential backoff
        m_RTO = qMin(m_RTO * 2, (quint64)3000);

        sendNextWindow();
    }
}

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

#ifdef DEBUG_TX_SOCKET
    qDebug() << "SlothTX:: <=== ACK with base " << base << "CWnd:" << m_congestionWindow;
    SlothPacketUtils::logBitMap(bitmap);
#endif

    bool lossDetected = false;
    quint32 newlyAckedPackets = 0;
    quint64 now = m_rttTimer->elapsed();

    // mark all packets having seq base - 1 as acked
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
                // hole in bitmap indicates loss, but we'll only consider this,
                // when RTO has been passed for this packet
                if (m_sendWindow.contains(seq)) {
                    quint64 age = now - m_sentTimestamp.value(seq, now);
                    if(age > m_RTO && !m_missingWindow.contains(seq)) {
                        lossDetected = true;
                        m_missingWindow.insert(seq);
                        m_stats.totalPacketsLost++;
                    }
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
#ifdef DEBUG_TX_SOCKET
            qDebug() << "Fast retransmit triggered for base:" << base;
#endif

            // Mark next expected packet as lost
            quint32 nextExpected = base;
            if (m_sendWindow.contains(nextExpected)) {
                m_missingWindow.insert(nextExpected);
                m_stats.totalPacketsLost++;
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
#ifdef DEBUG_TX_SOCKET
    qDebug() << "Loss detected! CWnd:" << m_congestionWindow << "-> ";
#endif

    // Multiplicative decrease
    m_slowStartThreshold = qMax(m_congestionWindow / 2, m_minWindow);

    // Different recovery strategies based on loss frequency
    quint64 timeSinceLastLoss = (m_lastLossTime > 0) ? (now - m_lastLossTime) : UINT64_MAX;

    if (timeSinceLastLoss < m_estimatedRTT * 4) {
        // Frequent losses - be more conservative
        m_congestionWindow = m_minWindow;
        m_recentLossCount++;
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Frequent loss detected, aggressive backoff";
#endif
    } else {
        // Isolated loss - moderate backoff
        m_congestionWindow = qMax(m_congestionWindow / 2, m_minWindow);
        m_recentLossCount = 1;
    }

    m_consecutiveGoodAcks = 0;
    m_lastLossTime = now;

#ifdef DEBUG_TX_SOCKET
    qDebug() << m_congestionWindow << "SSThresh:" << m_slowStartThreshold;
#endif
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

#ifdef DEBUG_TX_SOCKET
            qDebug() << "Bandwidth estimate:" << (m_estimatedBandwidth * 8 / 1000) << "Mbps";
#endif
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
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Slow Start: CWnd increased to" << m_congestionWindow;
#endif
    } else {
        // Congestion Avoidance: linear growth
        // Increase by 1 packet per RTT (approximated)
        if (m_consecutiveGoodAcks >= m_congestionWindow / 2) {
            m_congestionWindow++;
            m_consecutiveGoodAcks = 0;
#ifdef DEBUG_TX_SOCKET
            qDebug() << "Congestion Avoidance: CWnd increased to" << m_congestionWindow;
#endif
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
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Increased max window to" << m_maxWindow;
#endif
    } else if (m_recentLossCount > 3) {
        // Frequent losses - decrease max window
        m_maxWindow = qMax(m_maxWindow - 5, m_minWindow * 2);
#ifdef DEBUG_TX_SOCKET
        qDebug() << "Decreased max window to" << m_maxWindow;
#endif
    }

    // Decay recent loss count
    m_recentLossCount = m_recentLossCount / 2;
}

void SlothTxSocket::sendEOFPacket()
{
    if (m_transferCompleted) return; // Prevent duplicate FIN sending

    PacketHeader header(PacketType::FIN, m_activeSessionId, 0);
    QByteArray buffer = header.serialize(0);

    qInfo() << "Sending FIN packet to complete file transfer";
    transmitBuffer(buffer);
    m_stats.totalPacketsSent++;

    // Start FIN retry timer in case BYE response is lost
    m_finRetryTimer = new QTimer(this);
    m_finRetryCount = 0;

    connect(m_finRetryTimer, &QTimer::timeout, this, [=]() {
        if (++m_finRetryCount >= m_finRetryLimit) {
            // qWarning() << "FIN retry limit reached. Assuming transfer completed.";
            performTransferCleanup();
            m_finRetryTimer->stop();
            return;
        }

        qDebug() << "Retrying FIN packet... attempt" << m_finRetryCount;
        transmitBuffer(buffer);
        m_stats.totalPacketsSent++;
    });

    m_finRetryTimer->start(1000); // Retry every second

    // Safety timeout - force cleanup after 10 seconds regardless
    QTimer::singleShot(10000, this, [=]() {
        if (!m_transferCompleted) {
            qWarning() << "Transfer cleanup timeout - forcing completion";
            performTransferCleanup();
        }
    });
}

void SlothTxSocket::performTransferCleanup()
{
    if (m_transferCompleted) return; // Prevent double cleanup

    m_transferCompleted = true;

    // Stop all timers
    if (m_progressTimer) {
        m_progressTimer->stop();
        m_progressTimer->deleteLater();
        m_progressTimer = nullptr;
    }

    if (m_retransmitTimer) {
        m_retransmitTimer->stop();
        m_retransmitTimer->deleteLater();
        m_retransmitTimer = nullptr;
    }

    if (m_finRetryTimer) {
        m_finRetryTimer->stop();
        m_finRetryTimer->deleteLater();
        m_finRetryTimer = nullptr;
    }

    if (m_handshakeRetryTimer) {
        m_handshakeRetryTimer->stop();
        m_handshakeRetryTimer->deleteLater();
        m_handshakeRetryTimer = nullptr;
    }

    // Close file if still open
    if (m_file.isOpen()) {
        m_file.close();
    }

    // Print final statistics
    printTransmissionProgress();
    qInfo() << "=== TRANSFER COMPLETED SUCCESSFULLY ===";

    // Clean up state
    m_sendWindow.clear();
    m_missingWindow.clear();
    m_sentTimestamp.clear();
    m_lastRextTime.clear();
    m_duplicateAckCount.clear();

    // Reset session state
    m_sessionState = SessionState::NOTACTIVE;
    m_activeSessionId = 0;

    // Emit completion signal to application
    emit transferCompleted(true, m_filePath, m_stats.totalBytesSent,
                           QDateTime::currentMSecsSinceEpoch() - m_stats.transferStartTime);

    qInfo() << "Transmitter cleanup completed";
}

void SlothTxSocket::handleBye()
{
    QTime now = QTime::currentTime();
    int elapsedMs = m_progressStartTime.msecsTo(now);
    double elapsedSec = elapsedMs / 1000.0;

    double avgBytesPerSec = (elapsedSec > 0) ? m_stats.uniqueBytesSent / elapsedSec : 0;
    double avgMbps = (avgBytesPerSec * 8) / (1024 * 1024);

    double efficiency = (m_stats.totalBytesSent > 0) ?
                            (double(m_stats.uniqueBytesSent) / m_stats.totalBytesSent) * 100.0 : 0.0;

    qInfo() << "=== TX FINAL STATS ===";
    qInfo() << QString("Total Time: %1 seconds").arg(elapsedSec, 0, 'f', 2);
    qInfo() << QString("Average Speed: %1 Mbps (%2 KB/s)")
                   .arg(avgMbps, 0, 'f', 2)
                   .arg(avgBytesPerSec / 1024, 0, 'f', 1);
    qInfo() << QString("Final Efficiency: %1%").arg(efficiency, 0, 'f', 1);
    qInfo() << QString("Total Packets Sent: %1, Total Packets Lost: %2, Total Retransmission: %3")
                   .arg(m_stats.totalPacketsSent)
                   .arg(m_stats.totalPacketsLost)
                   .arg(m_stats.totalRetransmissions);
    qInfo() << "=====================";

    performTransferCleanup();
}
