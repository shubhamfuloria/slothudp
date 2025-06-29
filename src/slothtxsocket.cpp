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
    m_linkCapacityBps = 10000; // Start with 10Kbps instead of 1Mbps
    m_adaptiveChunkSize = 512;  // Smaller chunks for unreliable links
    m_congestionState = SLOW_START;
    m_cwndFloat = 4.0; // Start with 4 packets
    m_congestionWindow = 4;

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
    // Conservative but working settings for 3KB/s, 100ms RTT
    m_linkCapacityBps = 24000; // 3KB/s = 24Kbps
    m_adaptiveChunkSize = 512;

    // Start small and let it grow
    m_congestionWindow = 4; // Start with 4 packets
    m_cwndFloat = 4.0;
    m_ssthresh = 8;
    m_windowSize = 12;
    m_maxWindow = 20;

    m_estimatedRTT = 100;
    m_RTO = 300;

    // Disable pacing initially
    m_pacingInterval = 0;
    m_targetUtilization = 80;

    qDebug() << "Initialized for low-speed link: CWnd =" << m_congestionWindow;
}



void SlothTxSocket::adaptChunkSize()
{
    if (m_linkCapacityBps == 0) return;

    // CRITICAL FIX: Much more conservative chunk sizing
    quint32 optimalSize;

    // Base chunk size on actual measured bandwidth, not estimated
    if (m_linkCapacityBps < 20000) {        // < 20 Kbps (2.5 KB/s)
        optimalSize = 256;
    } else if (m_linkCapacityBps < 50000) { // < 50 Kbps (6.25 KB/s)
        optimalSize = 512;
    } else if (m_linkCapacityBps < 100000) { // < 100 Kbps (12.5 KB/s)
        optimalSize = 768;
    } else if (m_linkCapacityBps < 200000) { // < 200 Kbps (25 KB/s)
        optimalSize = 1024;
    } else {
        optimalSize = 1024; // Never exceed 1KB for low-speed links
    }

    // CRITICAL: Account for loss rate - smaller chunks for lossy links
    if (m_lossRate > 0.02) { // > 2% loss
        optimalSize = qMax(optimalSize / 2, m_minChunkSize);
    }

    // CRITICAL: Account for high RTT - don't increase chunk size
    if (m_estimatedRTT > 1000) { // High latency > 1 second
        optimalSize = qMin(optimalSize, (quint32)512); // Cap at 512 bytes
    }

    // Gradual adaptation - prevent sudden jumps
    quint32 currentSize = m_adaptiveChunkSize;
    if (optimalSize > currentSize) {
        optimalSize = qMin(optimalSize, currentSize + 128); // Increase by max 128 bytes
    } else if (optimalSize < currentSize) {
        optimalSize = qMax(optimalSize, currentSize - 128); // Decrease by max 128 bytes
    }

    m_adaptiveChunkSize = qBound(m_minChunkSize, optimalSize, m_maxChunkSize);
    m_chunkSize = m_adaptiveChunkSize;

    qDebug() << "Chunk size adapted from" << currentSize << "to" << m_adaptiveChunkSize
             << "for bandwidth" << (m_linkCapacityBps/1000.0) << "Kbps";
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
    if (elapsed < 2000) return; // Wait 2 seconds for stable measurement

    // Calculate actual goodput (successfully delivered bytes)
    double currentGoodput = (double)m_goodputBytes * 8000 / elapsed; // Convert to bps

    // Only update if we have meaningful data
    if (currentGoodput > 0 && m_goodputBytes > 0) {
        m_throughputSamples.enqueue(currentGoodput);
        if (m_throughputSamples.size() > 5) { // Shorter history for faster adaptation
            m_throughputSamples.dequeue();
        }

        double avgThroughput = 0;
        for (double sample : m_throughputSamples) {
            avgThroughput += sample;
        }
        avgThroughput /= m_throughputSamples.size();

        // CRITICAL FIX: Very conservative capacity estimation
        if (avgThroughput > 1000) { // Only update if > 1Kbps measured
            // Use measured goodput as capacity estimate with small buffer
            double newEstimate = avgThroughput * 1.1; // Only 10% headroom

            // Prevent sudden increases - gradual adaptation only
            if (newEstimate > m_linkCapacityBps * 1.5) {
                newEstimate = m_linkCapacityBps * 1.2; // Max 20% increase
            }

            // Smooth transition with more weight on current measurement
            m_linkCapacityBps = 0.3 * m_linkCapacityBps + 0.7 * newEstimate;
            m_linkCapacityBps = qMax(m_linkCapacityBps, 5000ULL); // Minimum 5Kbps
            m_linkCapacityBps = qMin(m_linkCapacityBps, 200000ULL); // Maximum 200Kbps for safety
        }
    }

    m_goodputBytes = 0;
    m_goodputTimer = now;

    adaptChunkSize();
    updatePacingInterval();
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
    if (m_linkCapacityBps == 0 || m_congestionWindow == 0) {
        m_pacingInterval = 100; // 100ms minimum interval for very slow links
        return;
    }

    // CRITICAL FIX: Very conservative pacing for low bandwidth
    quint64 targetBps = (m_linkCapacityBps * 60) / 100; // Use only 60% of measured capacity
    quint64 bytesPerPacket = m_adaptiveChunkSize + 60; // Account for all headers

    // Calculate time between packets in milliseconds
    m_pacingInterval = (bytesPerPacket * 8000) / targetBps;

    // CRITICAL FIX: Much longer intervals for stability
    m_pacingInterval = qBound((quint64)50, m_pacingInterval, (quint64)500); // 50ms to 500ms

    qDebug() << "Pacing interval updated to" << m_pacingInterval << "ms for"
             << (targetBps/1000.0) << "Kbps target, chunk size" << m_adaptiveChunkSize;
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
                   .arg(m_linkCapacityBps / 1000.0, 0, 'f', 1)
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

    // CRITICAL FIX: Detect repeated deadlock (checksum failure pattern)
    static int consecutiveDeadlocks = 0;
    bool isDeadlocked = (m_sendWindow.size() > 0 && m_sendWindow.size() == m_missingWindow.size());

    if (isDeadlocked) {
        consecutiveDeadlocks++;
        qWarning() << "Deadlock" << consecutiveDeadlocks << "detected! Window size:"
                   << m_sendWindow.size();

        // If we keep getting deadlocked on same packet, it's likely fragmentation
        if (consecutiveDeadlocks > 3) {
            qWarning() << "Repeated deadlock - likely fragmentation. Reducing chunk size.";

            // Aggressively reduce chunk size to prevent fragmentation
            m_adaptiveChunkSize = qMax(m_adaptiveChunkSize / 2, (quint32)256);
            m_chunkSize = m_adaptiveChunkSize;
            m_maxChunkSize = m_adaptiveChunkSize; // Prevent it from growing again

            // Clear the problematic packet and restart
            QList<quint32> deadlockedPackets = m_missingWindow.values();
            for (quint32 seq : deadlockedPackets) {
                qWarning() << "Deadlocked packet" << seq << " — forcing retransmit";

                // Don't remove from sendWindow — allow retransmission
                m_lastRextTime.remove(seq); // Force immediate retry
                m_lossDetectionCount.remove(seq); // Reset detection count

                // Mark as missing again
                m_missingWindow.insert(seq);
            }

            consecutiveDeadlocks = 0;

            qWarning() << "Reduced chunk size to" << m_adaptiveChunkSize
                       << "and cleared deadlocked packets";
        }

        // Send only one packet with longer delay during deadlock
        QThread::msleep(200); // 200ms delay between retransmissions
    } else {
        consecutiveDeadlocks = 0;
    }


    quint32 maxBurstSize = qMin((quint32)8, m_congestionWindow / 2);


    if (isDeadlocked) {
        qWarning() << "Deadlock detected! Implementing aggressive recovery";
        maxBurstSize = 1; // Send only one packet at a time during deadlock

        // FIX: Clear corrupted state and force recreation
        QList<quint32> deadlockedPackets = m_missingWindow.values();
        for (quint32 seq : deadlockedPackets) {
            m_lastRextTime.remove(seq); // Force immediate retransmission
            m_lossDetectionCount.remove(seq);
        }
    }

    quint32 effectiveWindow = getEffectiveWindowSize();
    int packetsToSend = 0;

    // CRITICAL FIX: Recreate packets instead of reusing buffers
    QList<quint32> missingList = m_missingWindow.values();
    std::sort(missingList.begin(), missingList.end());

    for (quint32 seq : missingList) {
        if (packetsToSend >= (int)maxBurstSize) break;

        quint64 now = m_rttTimer->elapsed();
        quint64 lastRext = m_lastRextTime.value(seq, 0);
        quint64 rextInterval = qMax((quint64)300, (quint64)(m_estimatedRTT * 2));

        if (lastRext == 0 || (now - lastRext) >= rextInterval) {
            // FIX: Recreate the packet instead of reusing buffer
            if (recreateAndRetransmitPacket(seq)) {
                m_stats.totalPacketsSent++;
                m_stats.totalRetransmissions++;
                m_lastRextTime[seq] = now;
                m_sentTimestamp[seq] = now;
                packetsToSend++;

                if (isDeadlocked) {
                    QThread::msleep(50); // Longer delay during deadlock recovery
                } else if (packetsToSend < (int)maxBurstSize) {
                    QThread::msleep(10);
                }
            } else {
                qWarning() << "Failed to recreate packet" << seq << "- removing from windows";
                // If we can't recreate the packet, remove it from tracking
                m_sendWindow.remove(seq);
                m_missingWindow.remove(seq);
                m_sentTimestamp.remove(seq);
                m_lastRextTime.remove(seq);
            }
        }
    }

    // Continue with new packets...
    int newPacketsSent = 0;
    while (m_nextSeqNum < m_baseSeqNum + effectiveWindow &&
           (packetsToSend + newPacketsSent) < (int)maxBurstSize &&
           !m_file.atEnd()) {

        QByteArray chunk = m_file.read(m_adaptiveChunkSize);
        if (chunk.isEmpty()) break;

        DataPacket packet;
        packet.header = PacketHeader(PacketType::DATA, m_nextSeqNum, chunk.size());
        // FIX: Ensure proper checksum calculation
        packet.header.checksum = qChecksum(chunk.constData(), chunk.size());
        packet.chunk = chunk;

        QByteArray buffer = SlothPacketUtils::serializePacket(packet);

        qDebug() << "Sending new packet" << m_nextSeqNum << "checksum:" << packet.header.checksum;
        transmitBuffer(buffer);
        m_stats.totalPacketsSent++;

        m_sendWindow[m_nextSeqNum] = buffer;
        m_sentTimestamp[m_nextSeqNum] = m_rttTimer->elapsed();
        m_retransmitCount[m_nextSeqNum] = 0;

        ++m_nextSeqNum;
        ++newPacketsSent;
        m_stats.uniqueBytesSent += chunk.size();

        if (newPacketsSent < (int)maxBurstSize - packetsToSend && !m_file.atEnd()) {
            QThread::msleep(5);
        }
    }

    qDebug() << "Sent" << packetsToSend << "retransmissions and" << newPacketsSent << "new packets";

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
    qint64 bytesToRead = qMin((qint64)m_adaptiveChunkSize, (qint64)(m_fileSize - filePosition));
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
    if (m_pacingQueue.isEmpty()) return;

    QByteArray buffer = m_pacingQueue.dequeue();
    transmitBuffer(buffer);
    m_stats.totalPacketsSent++;

    // Schedule next packet
    if (!m_pacingQueue.isEmpty()) {
        m_pacingTimer->start(m_pacingInterval);
    }
}


void SlothTxSocket::handleRetransmissions()
{
    quint64 now = m_rttTimer->elapsed();
    QList<quint32> timedOutPackets;

    for (auto it = m_sentTimestamp.begin(); it != m_sentTimestamp.end(); ++it) {
        quint32 seq = it.key();
        quint64 sentTime = it.value();

        quint64 timeout = qMax((quint64)(m_estimatedRTT * 3), (quint64)1000); // Longer timeout

        if (now - sentTime >= timeout) {
            timedOutPackets.append(seq);
        }
    }

    if (!timedOutPackets.isEmpty()) {
        qDebug() << "Timeout detected for" << timedOutPackets.size() << "packets";

        for (quint32 seq : timedOutPackets) {
            // Check retransmission count to prevent infinite loops
            int retryCount = m_retransmitCount.value(seq, 0);
            if (retryCount >= 10) {
                qWarning() << "Packet" << seq << "exceeded retry limit, dropping";
                m_sendWindow.remove(seq);
                m_missingWindow.remove(seq);
                m_sentTimestamp.remove(seq);
                m_lastRextTime.remove(seq);
                m_retransmitCount.remove(seq);
                continue;
            }

            m_retransmitCount[seq] = retryCount + 1;

            if (!m_actuallyLostPackets.contains(seq)) {
                markPacketAsLost(seq, "Timeout");
            }
            m_missingWindow.insert(seq);
            m_stats.totalTimeouts++;
        }

        // If we have too many consecutive timeouts, implement recovery
        if (m_stats.totalTimeouts > 50) {
            qWarning() << "Excessive timeouts detected, implementing recovery";
            handleDeadlockRecovery();
        }

        sendNextWindow();
    }
}


quint32 SlothTxSocket::getEffectiveWindowSize()
{
    // CRITICAL FIX: Use larger effective window
    quint32 effectiveWindow = qMin(m_congestionWindow, m_maxWindow);
    effectiveWindow = qMax(effectiveWindow, (quint32)15); // Minimum 15 packets

    // Consider bandwidth-delay product for optimal window sizing
    if (m_estimatedBandwidth > 0 && m_estimatedRTT > 0) {
        quint64 bdpBytes = (m_linkCapacityBps * m_estimatedRTT) / (8 * 1000);
        quint32 bdpPackets = (bdpBytes / m_chunkSize) + 1;

        // CRITICAL FIX: Use 4x BDP instead of 2x for better buffering
        quint32 targetWindow = bdpPackets * 4;
        effectiveWindow = qMax(effectiveWindow, targetWindow);
    }

    // CRITICAL FIX: Higher maximum window
    effectiveWindow = qMin(effectiveWindow, (quint32)40);

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

    // FIX: Don't be too aggressive on loss
    switch (m_congestionState) {
    case SLOW_START:
    case CONGESTION_AVOIDANCE:
        // Moderate reduction instead of halving
        m_ssthresh = qMax((quint32)(m_cwndFloat * 0.7), m_minWindow);
        m_fastRecoveryTarget = m_nextSeqNum;
        m_congestionState = FAST_RECOVERY;
        m_cwndFloat = m_ssthresh + 2; // Less aggressive inflation
        break;

    case BANDWIDTH_PROBING:
        m_congestionState = CONGESTION_AVOIDANCE;
        m_ssthresh = qMax((quint32)(m_cwndFloat * 0.8), m_minWindow);
        m_cwndFloat = m_ssthresh;
        break;

    case FAST_RECOVERY:
        // Only reduce if multiple consecutive losses
        if (now - m_lastLossTime < m_estimatedRTT * 2) {
            m_ssthresh = qMax(m_ssthresh / 2, m_minWindow);
            m_cwndFloat = m_ssthresh;
        }
        break;
    }

    m_lastLossTime = now;
    m_congestionWindow = qMax((quint32)m_cwndFloat, m_minWindow);

    // FIX: Don't increase RTO too aggressively
    m_RTO = qMin((quint64)(m_RTO * 1.5), (quint64)5000);

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



void SlothTxSocket::handleSuccessfulAck(quint32 newlyAckedPackets, quint64 now)
{
    m_consecutiveGoodAcks += newlyAckedPackets;
    m_goodputBytes += newlyAckedPackets * m_adaptiveChunkSize;

    switch (m_congestionState) {
    case SLOW_START:
        // Exponential increase
        m_cwndFloat += newlyAckedPackets;
        if (m_cwndFloat >= m_ssthresh) {
            m_congestionState = CONGESTION_AVOIDANCE;
            qDebug() << "Entering congestion avoidance, cwnd:" << m_cwndFloat;
        }
        break;

    case CONGESTION_AVOIDANCE:
        // Linear increase: 1 packet per RTT
        m_cwndFloat += (double)newlyAckedPackets / m_cwndFloat;

        // Probe for more bandwidth occasionally
        if (m_consecutiveGoodAcks > m_cwndFloat * 4 && m_lossRate < 0.01) {
            m_congestionState = BANDWIDTH_PROBING;
            qDebug() << "Starting bandwidth probing";
        }
        break;

    case BANDWIDTH_PROBING:
        // Aggressive increase to find capacity
        m_cwndFloat += newlyAckedPackets * 1.5;
        break;

    case FAST_RECOVERY:
        // Inflate window for each additional ACK
        m_cwndFloat += newlyAckedPackets;
        if (m_baseSeqNum >= m_fastRecoveryTarget) {
            m_congestionState = CONGESTION_AVOIDANCE;
            m_cwndFloat = m_ssthresh;
            qDebug() << "Exiting fast recovery";
        }
        break;
    }

    m_congestionWindow = (quint32)m_cwndFloat;

    // Adaptive maximum window based on BDP
    quint32 bdpWindow = calculateBDPWindow();
    m_congestionWindow = qMin(m_congestionWindow, bdpWindow);
    m_congestionWindow = qMax(m_congestionWindow, m_minWindow);
}

quint32 SlothTxSocket::calculateBDPWindow()
{
    if (m_linkCapacityBps == 0 || m_estimatedRTT == 0) {
        return m_maxWindow;
    }

    // BDP = (Bandwidth * RTT) / packet_size
    quint64 bdpBytes = (m_linkCapacityBps * m_estimatedRTT) / (8 * 1000); // Convert to bytes
    quint32 bdpPackets = (bdpBytes / m_adaptiveChunkSize) + 1;

    // Use 2x BDP for buffer, but cap at reasonable maximum
    return qMin(bdpPackets * 2, (quint32)256);
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
