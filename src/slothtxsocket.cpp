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

    qDebug() << "SlothTX:: packet ";
    packet.print();

    QByteArray buffer = SlothPacketUtils::serializePacket(packet);
    // send the buffer over to network

    transmitBuffer(buffer);
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

        case PacketType::BYE:
            // received has successfully received EOF packet and we should now close the connection
            // before that make sure that we've received acknowledgment of all packets
            handleBye();

        default:
            qDebug() << "SlothTx:: received unexpected packet, dropping...";
        }
    }
}

void SlothTxSocket::handleHandshakeAck(quint32 requestId)
{
    qDebug() << "Received handshake ack for request Id " << requestId;
    // verify if it's a valid request id and start file transfer

    initiateFileTransfer();
}

void SlothTxSocket::handleDataAck(PacketHeader header, QByteArray buffer)
{
    AckWindowPacket packet;
    SlothPacketUtils::deserializePacket(buffer, packet);

    qDebug() << "Handling data acknowledgement";
    packet.print();

    quint32 base = packet.baseSeqNum;
    QByteArray bitmap = packet.bitmap;

    for(int i = 0; i < bitmap.size(); i++) {
        quint8 byte = static_cast<quint8>(bitmap[i]);
        for(int bit = 0; bit < 8; bit++) {
            quint32 seq = base + i * 8 + bit;
            bool isAcked = byte & (1 << (7 - bit));
            if(isAcked) m_sendWindow.remove(seq);
        }
    }
    qDebug() << "Sliding window";
    //slide window
    while (!m_sendWindow.contains(m_baseSeqNum) && m_baseSeqNum < m_nextSeqNum) {
        ++m_baseSeqNum;
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
    if (!m_file.isOpen() || !m_file.isReadable()) {
        qWarning() << "File not open for reading!";
        return;
    }

    while ((m_nextSeqNum < m_baseSeqNum + m_windowSize) && !m_file.atEnd()) {
        QByteArray chunk = m_file.read(m_chunkSize);


        DataPacket packet;
        packet.header.sequenceNumber = m_nextSeqNum;
        packet.header.type = PacketType::DATA;
        packet.header.payloadSize = chunk.size();
        packet.header.checksum = qChecksum(chunk.constData(), packet.header.payloadSize);

        packet.chunk = chunk;


        QByteArray buffer = SlothPacketUtils::serializePacket(packet);
        qDebug() << "sending data packet with seq " << m_nextSeqNum;
        packet.header.print();
        transmitBuffer(buffer);

        m_sendWindow[m_nextSeqNum] = buffer;
        ++m_nextSeqNum;
    }

    if (m_file.atEnd()) {
        qDebug() << "Reached end of file (EOF). Waiting for ACKs before sending FIN.";

        qDebug() << "Sending EOF packet";
        sendEOFPacket();
    }
}

void SlothTxSocket::sendEOFPacket()
{
    PacketHeader header(PacketType::FIN, 0/*reqId*/, 0);
    header.checksum = 0;
    QByteArray buffer = header.serialize();

    transmitBuffer(buffer);
}

void SlothTxSocket::handleBye()
{
    // received has successfully
}

