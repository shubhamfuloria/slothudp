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
    packet.protocolVersion = 1;

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


            break;

        default:
            qDebug() << "SlothTx:: received unexpected packet, dropping...";
        }
    }
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
    m_windowSize = 4;

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
        packet.header.payloadSize = packet.chunk.size();
        packet.header.checksum = qChecksum(packet.chunk.constData(), packet.header.payloadSize);

        packet.chunk = chunk;


        QByteArray buffer = SlothPacketUtils::serializePacket(packet);

        transmitBuffer(buffer);

        m_sendWindow[m_nextSeqNum] = buffer;

        ++m_nextSeqNum;
    }

    if (m_file.atEnd()) {
        qDebug() << "Reached end of file (EOF). Waiting for ACKs before sending FIN.";
    }
}

