#include "include/slothtxsocket.h"
#include "include/slothpacketutils.h"

#include <QRandomGenerator>
#include <QIODevice>
#include <QDataStream>
#include <QFileInfo>
#include <QNetworkDatagram>

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

void SlothTxSocket::handleDataAck(PacketHeader header, QByteArray buffer)
{
    AckWindowPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);
    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

    qDebug() << "SlothTX:: <=== ACK with base " << base;
    SlothPacketUtils::logBitMap(bitmap);

    qDebug() << "m_sendWindow before ack calculation " << m_sendWindow.keys();
    qDebug() << "updating m_base from " << m_baseSeqNum << " to " << base - 1;
    m_baseSeqNum = base - 1;

    for(int i = 0; i < bitmap.size(); i++) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for(int bit = 0; bit < 8; bit++) {
            quint32 seq = base + i * 8 + bit;
            bool isAcked = byte & (1 << (7 - bit));
            if(isAcked) {
                m_sendWindow.remove(seq);
            }
        }
    }
    while (!m_sendWindow.contains(m_baseSeqNum) && m_baseSeqNum < m_nextSeqNum) {
        ++m_baseSeqNum;
    }
    qDebug() << "m_sendWindow after ack calculation " << m_sendWindow.keys();
    sendNextWindow();
}

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
                m_nackWindow.insert(seq);
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
    m_windowSize = 8;

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

    int packetSent = 0;

    // send nack items
    for(quint32 seq : qAsConst(m_nackWindow)) {
        // assuming nack packet is already present in m_sendWindow
        if(m_sendWindow.contains(seq)) {
            // this should be complete buffer including all checksum and all
            QByteArray buffer = m_sendWindow[seq];
            qDebug() << "SlothTX: DATA(n) seq " << seq << " ====> ";

            transmitBuffer(buffer);
            packetSent++;
        } else {
            qDebug() << "Packet not present in sendWindow, invalid ACK";
        }
    }
    m_nackWindow.clear();
    qDebug() << QString("m_nextSeqNum: %1, m_baseSeqNum: %2, m_windowSize: %3, packetSet: %4")
                    .arg(m_nextSeqNum).arg(m_baseSeqNum).arg(m_windowSize).arg(packetSent);
    while ((m_nextSeqNum < m_baseSeqNum + m_windowSize) && packetSent < m_windowSize && !m_file.atEnd()) {
        QByteArray chunk = m_file.read(m_chunkSize);


        DataPacket packet;
        packet.header = PacketHeader(PacketType::DATA, m_nextSeqNum, chunk.size());
        packet.header.checksum = qChecksum(chunk.constData(), packet.header.payloadSize);
        packet.chunk = chunk;

        QByteArray buffer = SlothPacketUtils::serializePacket(packet);

        qDebug() << "SlothTX: DATA seq " << packet.header.sequenceNumber << " ====> ";

        transmitBuffer(buffer);

        m_sendWindow[m_nextSeqNum] = buffer;
        ++m_nextSeqNum;
        ++packetSent;
    }

    if (m_file.atEnd()) {
        qDebug() << "Reached end of file (EOF). Waiting for ACKs before sending FIN.";

        qDebug() << "Sending EOF packet";
        sendEOFPacket();
    }
}

// void SlothTxSocket::sendPacket(DataPacket& packet)
// {

//     QByteArray buffer = SlothPacketUtils::serializePacket(packet);

//     transmitBuffer(buffer);
// }

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

