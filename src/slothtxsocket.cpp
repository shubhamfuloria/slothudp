#include "include/slothtxsocket.h"
#include "include/slothpacketutils.h"

#include <QRandomGenerator>
#include <QIODevice>
#include <QDataStream>
#include <QFileInfo>
#include <QNetworkDatagram>
#include <QElapsedTimer>
#include <QDateTime>
#include <QThread>

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
    m_stats = {};
    m_stats.transferStartTime = QDateTime::currentMSecsSinceEpoch();

    // FIX: Start with more reasonable assumptions for low-speed links
    m_linkCapacityBps = 640000; // Start with 10Kbps instead of 1Mbps
    m_adaptiveChunkSize = 512;  // Smaller chunks for unreliable links
    m_congestionState = SLOW_START;
    m_congestionWindow = 8;
    m_cwndFloat = 8.0;

    // FIX: More reasonable initial timeouts
    m_RTO = 1000; // 1 second instead of default
    m_estimatedRTT = 500; // Assume 500ms RTT initially

    initializeForLowSpeedLink();
    // Setup pacing timer
    m_pacingTimer = new QTimer(this);
    m_pacingTimer->setSingleShot(true);
    connect(m_pacingTimer, &QTimer::timeout, this, &SlothTxSocket::sendPacedPacket);

    // Setup progress timer
    m_progressTimer = new QTimer(this);
    connect(m_progressTimer, &QTimer::timeout, this, &SlothTxSocket::printTransmissionProgress);

    // Setup bandwidth estimation timer
    QTimer* bwTimer = new QTimer(this);
    connect(bwTimer, &QTimer::timeout, this, &SlothTxSocket::updateBandwidthEstimation);
    bwTimer->start(1000); // Update every second
}

void SlothTxSocket::initializeForLowSpeedLink()
{
    // FIXED: More realistic initial estimates for bandwidth range
    m_linkCapacityBps = 100000; // Start with 100Kbps (middle of range)
    m_adaptiveChunkSize = 1024;  // Larger chunks for better efficiency
    m_minChunkSize = 512;
    m_maxChunkSize = 1400;       // Near MTU size for efficiency
    m_chunkSize = m_adaptiveChunkSize;

    // FIXED: Larger initial window based on BDP
    // For 100Kbps, 200ms RTT: BDP = 2.5KB = ~3 packets of 1024 bytes
    // Use 4x BDP for optimal performance = 12 packets
    m_congestionWindow = 16;     // Start larger
    m_cwndFloat = 16.0;
    m_ssthresh = 32;
    m_minWindow = 8;             // Higher minimum
    m_maxWindow = 128;           // Much higher maximum
    m_windowSize = 32;

    // FIXED: Reasonable RTT assumptions
    m_estimatedRTT = 200;        // 200ms for low-speed links
    m_RTO = 600;                 // 3x RTT
    m_minRTT = 50;

    // FIXED: Enable proper pacing
    m_pacingInterval = 50;       // Start with 50ms between packets
    m_targetUtilization = 85;    // Target 85% utilization

    qDebug() << "Initialized for 10Kbps-1Mbps range: CWnd =" << m_congestionWindow
             << "ChunkSize =" << m_adaptiveChunkSize
             << "Pacing =" << m_pacingInterval << "ms";
}






void SlothTxSocket::adaptChunkSize()
{
    if (m_linkCapacityBps == 0) return;

    // FIXED: Adaptive chunk sizing based on bandwidth and loss
    quint32 optimalSize;

    if (m_linkCapacityBps < 50000) {        // < 50Kbps
        optimalSize = 512;
    } else if (m_linkCapacityBps < 200000) { // 50-200Kbps
        optimalSize = 1024;
    } else {                                 // > 200Kbps
        optimalSize = 1400;
    }

    // Reduce size if experiencing loss
    if (m_lossRate > 0.02) {      // > 2% loss
        optimalSize = qMax(optimalSize / 2, m_minChunkSize);
    } else if (m_lossRate > 0.05) { // > 5% loss
        optimalSize = m_minChunkSize;
    }

    // Gradual adaptation to prevent oscillation
    static quint64 lastAdaptTime = 0;
    quint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastAdaptTime < 10000) return; // Wait 10 seconds
    lastAdaptTime = now;

    if (optimalSize != m_adaptiveChunkSize) {
        // Gradual change - max 256 bytes per adaptation
        if (abs((int)optimalSize - (int)m_adaptiveChunkSize) > 256) {
            optimalSize = m_adaptiveChunkSize +
                          ((optimalSize > m_adaptiveChunkSize) ? 256 : -256);
        }

        m_adaptiveChunkSize = qBound(m_minChunkSize, optimalSize, m_maxChunkSize);
        m_chunkSize = m_adaptiveChunkSize;

        qDebug() << "Chunk size adapted to" << m_adaptiveChunkSize
                 << "for bandwidth" << (m_linkCapacityBps / 1000.0) << "Kbps";
    }
}



void SlothTxSocket::updateBandwidthEstimation()
{
    quint64 now = QDateTime::currentMSecsSinceEpoch();

    if (m_goodputTimer == 0) {
        m_goodputTimer = now;
        m_goodputBytes = 0;
        return;
    }

    quint64 elapsed = now - m_goodputTimer;
    if (elapsed < 3000) return; // Wait 3 seconds for stable measurement

    // Calculate actual goodput (successfully delivered bytes)
    double currentGoodput = (double)m_goodputBytes * 8000 / elapsed;

    if (currentGoodput > 5000 && m_goodputBytes > 0) { // Only if > 5Kbps measured
        m_throughputSamples.enqueue(currentGoodput);
        if (m_throughputSamples.size() > 10) {
            m_throughputSamples.dequeue();
        }

        double avgThroughput = 0;
        for (double sample : m_throughputSamples) {
            avgThroughput += sample;
        }
        avgThroughput /= m_throughputSamples.size();

        // FIXED: More aggressive capacity estimation with proper bounds
        if (avgThroughput > 8000) { // Only update if > 8Kbps measured
            // Use measured goodput with reasonable headroom
            double newEstimate = avgThroughput * 1.3; // 30% headroom for burstiness

            // Allow faster adaptation upward, slower downward
            double alpha = (newEstimate > m_linkCapacityBps) ? 0.3 : 0.1;
            m_linkCapacityBps = (1.0 - alpha) * m_linkCapacityBps + alpha * newEstimate;

            // FIXED: Proper bounds for target range
            m_linkCapacityBps = qBound(8000ULL, m_linkCapacityBps, 1200000ULL);
        }
    }

    m_goodputBytes = 0;
    m_goodputTimer = now;

    adaptChunkSize();
    updatePacingInterval();
    updateWindowSize();
}



void SlothTxSocket::updateWindowSize()
{
    // FIXED: Calculate optimal window size based on BDP
    if (m_linkCapacityBps > 0 && m_estimatedRTT > 0) {
        // Bandwidth-Delay Product in bytes
        quint64 bdpBytes = (m_linkCapacityBps * m_estimatedRTT) / (8 * 1000);

        // Convert to packets
        quint32 bdpPackets = qMax((quint32)(bdpBytes / m_adaptiveChunkSize), 1U);

        // Use 2-4x BDP based on link quality
        quint32 multiplier = (m_lossRate < 0.01) ? 4 : 2;
        quint32 targetWindow = bdpPackets * multiplier;

        // Gradual window adjustment
        if (targetWindow > m_congestionWindow && m_lossRate < 0.01) {
            m_congestionWindow = qMin(targetWindow, m_maxWindow);
        }

        qDebug() << "BDP Window: BDP=" << bdpPackets
                 << "Target=" << targetWindow
                 << "Current=" << m_congestionWindow;
    }
}

void SlothTxSocket::handleBandwidthTransition()
{
    // Detect sudden bandwidth changes
    if (m_throughputSamples.size() >= 3) {
        QList<double> samples;
        for (auto val : m_throughputSamples) {
            samples.append(static_cast<double>(val));
        }
        double recent = samples.last();
        double previous = samples[samples.size()-2];

        // If bandwidth suddenly increased by more than 3x
        if (recent > previous * 3 && previous > 0) {
            qWarning() << "Sudden bandwidth increase detected! Applying conservative scaling";

            // Temporarily reduce congestion window to prevent overwhelming receiver
            m_congestionWindow = qMax(m_congestionWindow / 2, m_minWindow);
            m_cwndFloat = m_congestionWindow;

            // Force longer pacing interval temporarily
            m_pacingInterval = qMax(m_pacingInterval, (quint64)25);

            // Reset to slow start to gradually adapt
            m_congestionState = SLOW_START;
            m_ssthresh = m_congestionWindow * 2;
        }
    }
}


void SlothTxSocket::updatePacingInterval()
{
    if (m_linkCapacityBps == 0 || m_adaptiveChunkSize == 0) {
        m_pacingInterval = 100;
        return;
    }

    // FIXED: Proper pacing calculation
    double bitsPerPacket = (m_adaptiveChunkSize + 60) * 8; // Include overhead
    double effectiveBps = m_linkCapacityBps * (m_targetUtilization / 100.0);

    // Calculate time between packets in milliseconds
    double intervalMs = (bitsPerPacket * 1000.0) / effectiveBps;

    // FIXED: Reasonable pacing bounds
    m_pacingInterval = qBound(5ULL, (quint64)intervalMs, 500ULL);

    // Adaptive pacing based on congestion window
    if (m_congestionWindow > 16) {
        // Faster pacing for larger windows
        m_pacingInterval = qMax((quint64)(m_pacingInterval * 0.7), 5ULL);
    }

    qDebug() << "Pacing updated:" << m_pacingInterval << "ms"
             << "(capacity:" << (m_linkCapacityBps / 1000.0) << "Kbps)";
}



void SlothTxSocket::handleChecksumFailures()
{
    // If we're getting repeated NACKs for the same packet, it's likely checksum failure
    // Reduce chunk size more aggressively

    if (m_deadlockCount > 3) { // Repeated deadlock = likely fragmentation
        qWarning() << "Repeated deadlock detected - likely fragmentation. Reducing chunk size.";

        // Aggressively reduce chunk size
        m_adaptiveChunkSize = qMax(m_adaptiveChunkSize / 2, m_minChunkSize);
        m_chunkSize = m_adaptiveChunkSize;

        // Reset congestion window
        m_congestionWindow = 1;
        m_cwndFloat = 1.0;

        // Force longer pacing
        m_pacingInterval = qMax(m_pacingInterval, (quint64)100);

        qWarning() << "Reduced chunk size to" << m_adaptiveChunkSize
                   << "due to suspected fragmentation";
    }
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
    qInfo() << QString("Link Capacity: %1 Kbps, Chunk Size: %2 bytes")
                   .arg(m_linkCapacityBps / 1000.0)
                   .arg(m_adaptiveChunkSize);
    qInfo() << QString("Congestion State: %1, Loss Rate: %2%")
                   .arg(m_congestionState)
                   .arg(m_lossRate * 100, 0, 'f', 2);
    qInfo() << QString("Pacing: %1ms interval, Queue: %2 packets")
                   .arg(m_pacingInterval)
                   .arg(m_pacingQueue.size());

    qInfo() << QString("Debug: Actually lost packets: %1, Detection events: %2")
                   .arg(m_actuallyLostPackets.size())
                   .arg(m_stats.totalPacketsLost);
    qInfo() << QString("Debug: Send window size: %1, Missing window: %2")
                   .arg(m_sendWindow.size())
                   .arg(m_missingWindow.size());

    // Calculate theoretical maximum
    double theoreticalMaxKBps = (m_linkCapacityBps / 8.0) / 1024.0;
    double utilizationPercent = (bytesPerSec / 1024.0) / theoreticalMaxKBps * 100.0;
    qInfo() << QString("Link utilization: %1% of %2 KB/s theoretical max")
                   .arg(utilizationPercent, 0, 'f', 1)
                   .arg(theoreticalMaxKBps, 0, 'f', 1);

    qInfo() << QString("Dynamic scaling: CWND=%1, Chunk=%2, Pacing=%3ms, LinkCap=%.1f Kbps")
                   .arg(m_congestionWindow)
                   .arg(m_adaptiveChunkSize)
                   .arg(m_pacingInterval)
                   .arg(m_linkCapacityBps / 1000.0, 0, 'f', 1);

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

    initiateFileTransfer();
}

void SlothTxSocket::handleNack(PacketHeader header, QByteArray buffer)
{
    NackPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);

    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

    qDebug() << "NACK received - base:" << base << "bitmap size:" << bitmap.size()
             << "our base:" << m_baseSeqNum;

    // Only process NACKs for packets we actually sent
    for (int i = 0; i < bitmap.size(); ++i) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for (int bit = 0; bit < 8; ++bit) {
            quint32 seq = base + i * 8 + bit;
            bool isNacked = byte & (1 << (7 - bit));

            // CRITICAL: Only NACK packets that are actually in our send window
            if (isNacked && seq >= m_baseSeqNum) {
                m_missingWindow.insert(seq);
                qDebug() << "NACK for packet" << seq;

                // Trigger recreate if not present
                if (!m_sendWindow.contains(seq)) {
                    recreateAndRetransmitPacket(seq);
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
    qDebug() << "sendNextWindow: base=" << m_baseSeqNum << "next=" << m_nextSeqNum
             << "send_window=" << m_sendWindow.size() << "missing=" << m_missingWindow.size();

    // FIXED: Prevent deadlock with more intelligent recovery
    static int deadlockCount = 0;
    bool isDeadlocked = (m_sendWindow.size() > 0 &&
                         m_sendWindow.size() == m_missingWindow.size() &&
                         m_sendWindow.size() >= 3);

    if (isDeadlocked) {
        deadlockCount++;
        if (deadlockCount > 5) {
            qWarning() << "Persistent deadlock detected - implementing recovery";
            handleDeadlockRecovery();
            deadlockCount = 0;
            return;
        }
    } else {
        deadlockCount = 0;
    }

    // FIXED: More aggressive burst sending
    quint32 maxBurstSize = qMin(m_congestionWindow, (quint32)20);
    if (isDeadlocked) {
        maxBurstSize = 2; // Conservative during deadlock
    }

    quint32 effectiveWindow = getEffectiveWindowSize();
    int packetsToSend = 0;

    // Handle retransmissions first
    QList<quint32> missingList = m_missingWindow.values();
    std::sort(missingList.begin(), missingList.end());

    for (quint32 seq : missingList) {
        if (packetsToSend >= (int)maxBurstSize) break;

        quint64 now = m_rttTimer->elapsed();
        quint64 lastRext = m_lastRextTime.value(seq, 0);
        quint64 rextInterval = qMax((quint64)m_RTO, (quint64)200);

        if (lastRext == 0 || (now - lastRext) >= rextInterval) {
            if (recreateAndRetransmitPacket(seq)) {
                m_stats.totalPacketsSent++;
                m_stats.totalRetransmissions++;
                m_lastRextTime[seq] = now;
                m_sentTimestamp[seq] = now;
                packetsToSend++;

                // FIXED: Use pacing for retransmissions too
                if (packetsToSend < (int)maxBurstSize && !isDeadlocked) {
                    if (m_pacingInterval > 0) {
                        QThread::msleep(m_pacingInterval / 2); // Faster retransmit pacing
                    }
                }
            }
        }
    }

    // Send new packets
    int newPacketsSent = 0;
    while (m_nextSeqNum < m_baseSeqNum + effectiveWindow &&
           (packetsToSend + newPacketsSent) < (int)maxBurstSize &&
           !m_file.atEnd()) {

        QByteArray chunk = m_file.read(m_adaptiveChunkSize);
        if (chunk.isEmpty()) break;

        DataPacket packet;
        packet.header = PacketHeader(PacketType::DATA, m_nextSeqNum, chunk.size());
        packet.header.checksum = qChecksum(chunk.constData(), chunk.size());
        packet.chunk = chunk;

        QByteArray buffer = SlothPacketUtils::serializePacket(packet);

        // FIXED: Direct transmission with proper pacing
        if (m_pacingInterval > 0 && newPacketsSent > 0) {
            QThread::msleep(m_pacingInterval);
        }

        transmitBuffer(buffer);
        m_stats.totalPacketsSent++;

        m_sendWindow[m_nextSeqNum] = buffer;
        m_sentTimestamp[m_nextSeqNum] = m_rttTimer->elapsed();
        m_retransmitCount[m_nextSeqNum] = 0;

        ++m_nextSeqNum;
        ++newPacketsSent;
        m_stats.uniqueBytesSent += chunk.size();
    }

    qDebug() << "Sent" << packetsToSend << "retransmissions and"
             << newPacketsSent << "new packets (pacing:" << m_pacingInterval << "ms)";

    if (m_file.atEnd() && m_sendWindow.empty()) {
        sendEOFPacket();
        if (m_progressTimer) {
            m_progressTimer->stop();
            printTransmissionProgress();
        }
    }
}



bool SlothTxSocket::recreateAndRetransmitPacket(quint32 seq)
{
    // Calculate file position for this sequence number
    qint64 filePosition = seq * m_adaptiveChunkSize;

    if (filePosition >= m_fileSize) {
        qWarning() << "Packet" << seq << "beyond file size, cannot recreate";
        return false;
    }

    // Save current file position
    qint64 currentPos = m_file.pos();

    // Seek to the packet's data position
    if (!m_file.seek(filePosition)) {
        qWarning() << "Failed to seek to position" << filePosition << "for packet" << seq;
        m_file.seek(currentPos); // Restore position
        return false;
    }

    // Read the chunk data
    // qint64 bytesToRead = qMin((qint64)m_adaptiveChunkSize, (qint64)(m_fileSize - filePosition));
    qint64 remaining = (qint64)(m_fileSize - filePosition);
    if (remaining < 0) remaining = 0;
    qint64 bytesToRead = qMin((qint64)m_adaptiveChunkSize, remaining);
    QByteArray chunk = m_file.read(bytesToRead);

    // Restore file position
    m_file.seek(currentPos);

    if (chunk.isEmpty()) {
        qWarning() << "Failed to read chunk for packet" << seq;
        return false;
    }

    // Recreate the packet with fresh headers and checksum
    DataPacket packet;
    packet.header = PacketHeader(PacketType::DATA, seq, chunk.size());
    packet.header.checksum = qChecksum(chunk.constData(), chunk.size());
    packet.chunk = chunk;

    QByteArray buffer = SlothPacketUtils::serializePacket(packet);

    qDebug() << "Recreating packet" << seq << "size:" << chunk.size()
             << "checksum:" << packet.header.checksum << "pos:" << filePosition;

    // Update send window with fresh buffer
    m_sendWindow[seq] = buffer;

    // Transmit the recreated packet
    bool success = transmitBuffer(buffer);
    qDebug() << "Recreated packet" << seq
             << "Checksum:" << packet.header.checksum
             << "First byte:" << (int)(unsigned char)chunk[0]
             << "File offset:" << filePosition;

    if (success) {
        qDebug() << "Successfully retransmitted recreated packet" << seq;
    } else {
        qWarning() << "Failed to transmit recreated packet" << seq;
    }

    return success;
}


void SlothTxSocket::sendPacedPacket()
{
    int packetsSent = 0;
    int maxBurst = m_congestionWindow;

    while (!m_pacingQueue.isEmpty() &&
           m_sendWindow.size() < m_congestionWindow &&
           packetsSent < maxBurst)
    {
        QByteArray buffer = m_pacingQueue.dequeue();
        transmitBuffer(buffer);
        m_stats.totalPacketsSent++;
        packetsSent++;
    }

    // If more to send, continue pacing
    if (!m_pacingQueue.isEmpty()) {
        m_pacingTimer->start(m_pacingInterval);
    }
}



void SlothTxSocket::handleRetransmissions()
{
    const quint64 now = m_rttTimer->elapsed();

    // FIXED: More reasonable timeout calculation
    const quint64 baseTimeout = qMax((quint64)(m_RTO), (quint64)500);

    QList<quint32> timedOutPackets;

    for (auto it = m_sentTimestamp.begin(); it != m_sentTimestamp.end(); ++it) {
        const quint32 seq = it.key();
        const quint64 sentTime = it.value();

        if (now - sentTime >= baseTimeout) {
            timedOutPackets.append(seq);
        }
    }

    if (timedOutPackets.isEmpty()) return;

    qDebug() << "[Timeout] Detected" << timedOutPackets.size() << "packets"
             << "RTO:" << baseTimeout << "ms";

    for (quint32 seq : timedOutPackets) {
        int retryCount = m_retransmitCount.value(seq, 0);

        // FIXED: Higher retry limit for low-speed links
        if (retryCount >= 15) {
            qWarning() << "Dropping packet" << seq << "after" << retryCount << "retries";
            m_sendWindow.remove(seq);
            m_missingWindow.remove(seq);
            m_sentTimestamp.remove(seq);
            m_lastRextTime.remove(seq);
            m_retransmitCount.remove(seq);
            continue;
        }

        m_retransmitCount[seq] = retryCount + 1;

        // FIXED: Only mark as lost after multiple timeouts
        if (retryCount >= 3) {
            if (!m_actuallyLostPackets.contains(seq)) {
                markPacketAsLost(seq, "Multiple timeouts");
            }
            m_missingWindow.insert(seq);
        }

        m_stats.totalTimeouts++;
    }

    // FIXED: Less aggressive congestion response to timeouts
    if (timedOutPackets.size() > 5) {
        handleLossEvent(now);
    }
}



quint32 SlothTxSocket::getEffectiveWindowSize()
{
    // FIXED: More generous effective window
    quint32 effectiveWindow = qMin(m_congestionWindow, m_maxWindow);
    effectiveWindow = qMax(effectiveWindow, (quint32)8); // Minimum 8 packets

    // Consider bandwidth-delay product
    if (m_linkCapacityBps > 0 && m_estimatedRTT > 0) {
        quint64 bdpBytes = (m_linkCapacityBps * m_estimatedRTT) / (8 * 1000);
        quint32 bdpPackets = (bdpBytes / m_chunkSize) + 1;

        // Use 3x BDP for good performance
        quint32 targetWindow = bdpPackets * 3;
        effectiveWindow = qMax(effectiveWindow, targetWindow);
    }

    // FIXED: Reasonable maximum window
    effectiveWindow = qMin(effectiveWindow, (quint32)128);

    return effectiveWindow;
}

void SlothTxSocket::handleDeadlockRecovery()
{
    if (m_sendWindow.size() > 0 && m_sendWindow.size() == m_missingWindow.size()) {
        qWarning() << "Implementing deadlock recovery protocol";

        // Reset congestion control to minimum
        m_congestionWindow = 1;
        m_cwndFloat = 1.0;
        m_congestionState = SLOW_START;

        // Clear all timing information to force immediate retransmission
        m_lastRextTime.clear();
        m_lossDetectionCount.clear();

        // Increase RTO significantly
        m_RTO = qMin((quint64)5000, m_RTO * 2);

        // Force recreation of all packets in missing window
        QList<quint32> missingPackets = m_missingWindow.values();
        for (quint32 seq : missingPackets) {
            qDebug() << "Forcing recreation of deadlocked packet" << seq;
            recreateAndRetransmitPacket(seq);
            QThread::msleep(100); // Significant delay between packets
        }
    }
}

void SlothTxSocket::handleDataAck(PacketHeader header, QByteArray buffer)
{
    AckWindowPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);
    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

    bool lossDetected = false;
    quint32 newlyAckedPackets = 0;
    quint64 now = m_rttTimer->elapsed();

    qDebug() << "ACK received - base:" << base << "our base:" << m_baseSeqNum
             << "missing:" << m_missingWindow.size() << "send:" << m_sendWindow.size();

    // CRITICAL FIX: Always advance base if receiver has moved forward
    if (base > m_baseSeqNum) {
        qDebug() << "Advancing base from" << m_baseSeqNum << "to" << base;

        // Remove all packets up to the new base
        for (quint32 i = m_baseSeqNum; i < base; i++) {
            if (m_sendWindow.contains(i)) {
                updateRTTAndBandwidth(i, now);
                m_sendWindow.remove(i);
                newlyAckedPackets++;
                m_bytesAcked += m_chunkSize;
                qDebug() << "ACK: Removed packet" << i << "from send window";
            }

            // CRITICAL: Remove from missing window too
            if (m_missingWindow.contains(i)) {
                m_missingWindow.remove(i);
                qDebug() << "ACK: Removed packet" << i << "from missing window";
            }

            // Remove from lost packets
            if (m_actuallyLostPackets.contains(i)) {
                m_actuallyLostPackets.remove(i);
                m_stats.totalPacketsLost--;
                qDebug() << "ACK: Packet" << i << "was recovered";
            }

            // Clean up tracking data
            m_sentTimestamp.remove(i);
            m_lastRextTime.remove(i);
            m_lossDetectionCount.remove(i);
        }

        m_baseSeqNum = base;
    }

    // Process selective ACKs from bitmap
    for (int i = 0; i < bitmap.size(); i++) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for (int bit = 0; bit < 8; bit++) {
            quint32 seq = base + i * 8 + bit;
            bool isAcked = byte & (1 << (7 - bit));

            if (isAcked && seq >= base) {
                // Packet successfully received
                if (m_sendWindow.contains(seq)) {
                    updateRTTAndBandwidth(seq, now);
                    m_sendWindow.remove(seq);
                    newlyAckedPackets++;
                    m_bytesAcked += m_chunkSize;
                    qDebug() << "SACK: Removed packet" << seq << "from send window";
                }

                // Remove from missing window
                if (m_missingWindow.contains(seq)) {
                    m_missingWindow.remove(seq);
                    qDebug() << "SACK: Removed packet" << seq << "from missing window";
                }

                // Remove from lost packets
                if (m_actuallyLostPackets.contains(seq)) {
                    m_actuallyLostPackets.remove(seq);
                    m_stats.totalPacketsLost--;
                    qDebug() << "SACK: Packet" << seq << "was recovered";
                }
            }
        }
    }

    qDebug() << "After ACK processing: send_window=" << m_sendWindow.size()
             << "missing_window=" << m_missingWindow.size()
             << "newly_acked=" << newlyAckedPackets;

    // Update congestion control
    if (newlyAckedPackets > 0) {
        handleSuccessfulAck(newlyAckedPackets, now);
    }

    // updateBandwidthEstimate(now);
    updateBandwidthEstimation();
    sendNextWindow();
}

void SlothTxSocket::handleLossEvent(quint64 now)
{
    updateLossRate();

    // FIXED: Less aggressive loss response for low-speed links
    switch (m_congestionState) {
    case SLOW_START:
    case CONGESTION_AVOIDANCE:
        // Moderate reduction (80% instead of 50%)
        m_ssthresh = qMax((quint32)(m_cwndFloat * 0.8), m_minWindow);
        m_fastRecoveryTarget = m_nextSeqNum;
        m_congestionState = FAST_RECOVERY;
        m_cwndFloat = m_ssthresh + 3;
        break;

    case BANDWIDTH_PROBING:
        m_congestionState = CONGESTION_AVOIDANCE;
        m_ssthresh = qMax((quint32)(m_cwndFloat * 0.85), m_minWindow);
        m_cwndFloat = m_ssthresh;
        break;

    case FAST_RECOVERY:
        // Only reduce if repeated losses
        if (now - m_lastLossTime < m_estimatedRTT) {
            m_ssthresh = qMax(m_ssthresh * 4 / 5, m_minWindow);
            m_cwndFloat = m_ssthresh;
        }
        break;
    }

    m_lastLossTime = now;
    m_congestionWindow = qMax((quint32)m_cwndFloat, m_minWindow);

    // FIXED: Moderate RTO increase
    m_RTO = qMin((quint64)(m_RTO * 1.25), (quint64)3000);
}

void SlothTxSocket::updateLossRate()
{
    if (m_stats.totalPacketsSent == 0) {
        m_lossRate = 0.0;
        return;
    }

    // Use only actually lost packets (not detection events)
    double currentLossRate = (double)m_actuallyLostPackets.size() / m_stats.totalPacketsSent;
    currentLossRate = qMin(currentLossRate, 1.0); // Cap at 100%

    m_lossRateSamples.enqueue(currentLossRate);
    if (m_lossRateSamples.size() > 5) {
        m_lossRateSamples.dequeue();
    }

    double sum = 0;
    for (double rate : m_lossRateSamples) {
        sum += rate;
    }
    m_lossRate = sum / m_lossRateSamples.size();

    qDebug() << "Loss rate updated:" << (m_lossRate * 100) << "% ("
             << m_actuallyLostPackets.size() << "/" << m_stats.totalPacketsSent << ")";
}

void SlothTxSocket::updateRTTAndBandwidth(quint32 seq, quint64 now)
{
    if (m_sentTimestamp.contains(seq)) {
        quint64 sentTime = m_sentTimestamp.value(seq);
        quint64 rtt = now - sentTime;

        if (rtt > 2000) return; // it may be noise

        // Update RTT statistics
        m_rttSamples.enqueue(rtt);
        if (m_rttSamples.size() > 20) {
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
        m_RTO = qBound((quint64)200, m_RTO, (quint64)3000); // Lower bound 200ms


        m_sentTimestamp.remove(seq);
    }
}



void SlothTxSocket::handleSuccessfulAck(quint32 newlyAckedPackets, quint64 now)
{
    m_consecutiveGoodAcks += newlyAckedPackets;
    m_goodputBytes += newlyAckedPackets * m_adaptiveChunkSize;

    // FIXED: More aggressive window growth for low-speed links
    switch (m_congestionState) {
    case SLOW_START:
        // Faster exponential increase
        m_cwndFloat += newlyAckedPackets * 1.5;
        if (m_cwndFloat >= m_ssthresh) {
            m_congestionState = CONGESTION_AVOIDANCE;
            qDebug() << "Entering congestion avoidance, cwnd:" << m_cwndFloat;
        }
        break;

    case CONGESTION_AVOIDANCE:
        // Faster linear increase
        m_cwndFloat += (double)newlyAckedPackets * 2.0 / m_cwndFloat;

        // Probe for bandwidth more aggressively
        if (m_consecutiveGoodAcks > m_cwndFloat * 2 && m_lossRate < 0.005) {
            m_congestionState = BANDWIDTH_PROBING;
            qDebug() << "Starting bandwidth probing";
        }
        break;

    case BANDWIDTH_PROBING:
        // Very aggressive increase
        m_cwndFloat += newlyAckedPackets * 2.0;
        break;

    case FAST_RECOVERY:
        m_cwndFloat += newlyAckedPackets;
        if (m_baseSeqNum >= m_fastRecoveryTarget) {
            m_congestionState = CONGESTION_AVOIDANCE;
            m_cwndFloat = m_ssthresh;
            qDebug() << "Exiting fast recovery";
        }
        break;
    }

    m_congestionWindow = qBound(m_minWindow, (quint32)m_cwndFloat, m_maxWindow);
}

quint32 SlothTxSocket::calculateBDPWindow()
{
    if (m_linkCapacityBps == 0 || m_estimatedRTT == 0)
        return m_maxWindow;

    quint64 bdpBytes = (m_linkCapacityBps * m_estimatedRTT) / (8 * 1000);
    quint32 bdpPackets = bdpBytes / m_adaptiveChunkSize;

    return qBound(4U, bdpPackets * 2, 256U); // Use 2x BDP
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
            qWarning() << "FIN retry limit reached. Assuming transfer completed.";
            performTransferCleanup();
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

void SlothTxSocket::resetLossTracking() {
    m_stats.totalPacketsLost = 0;
    m_actuallyLostPackets.clear();
    m_lossDetectionCount.clear();
    m_missingWindow.clear();
}

void SlothTxSocket::markPacketAsLost(quint32 seq, const QString& reason) {
    if (!m_actuallyLostPackets.contains(seq)) {
        m_actuallyLostPackets.insert(seq);
        m_stats.totalPacketsLost++;
        qDebug() << "Packet" << seq << "marked as lost:" << reason;
    }
}



void SlothTxSocket::handleBye()
{
    qInfo() << "Received BYE confirmation from receiver";
    performTransferCleanup();
}
