#ifndef SLOTHRXSOCKET_H
#define SLOTHRXSOCKET_H

#include <QObject>
#include <QUdpSocket>
#include <QFile>

#include <include/types.h>

class SlothRxSocket : public QUdpSocket
{
    Q_OBJECT

public:

    SlothRxSocket();

private:
    /**
     * @brief acknowledgeTxRequest: Creates and sends acknowledge packet
     *        to m_txAddress with requestId
     * @param requestId
     */
    bool acknowledgeTxRequest(quint32 requestId);

    /**
     * @brief handleHandshakePacket
     * @param buffer: buffer without packet header, HandshakePacket
     */
    void handleHandshakePacket(QByteArray buffer);


    void handlePacket(PacketHeader header, QByteArray payload);

    /**
     * @brief transmitBuffer: Writes the buffer to the network to m_txAddress
     * @param buffer
     */
    bool transmitBuffer(const QByteArray& buffer);


    void sendAcknowledgement();

    /**
     * @brief generateAckBitmap: Creates a ack bitmap starting from base. eg. if packet 3, 4, 6, 7 has been received
     *                           base will be 3 and bitmap will be 11011 ( 27 )
     * @param base
     * @param windowSize
     * @return
     */
    QByteArray generateAckBitmap(quint32 base, int windowSize);
    void sayByeToPeer(); // and tear down connections


    QHostAddress m_txAddress;
    quint16 m_txPort;


    QString m_filePath;
    QFile m_file;

    int m_baseAckSeqNum;
    int m_baseWriteSeqNum;
    int m_untrackedCount;
    QMap<quint32, QByteArray>m_recvWindow;
    QSet<quint32> m_receivedSeqNums;

private slots:
    void handleReadyRead();

signals:
    void on_fileTxRequest(QString fileName, quint64 fileSize, QString hostAddress);
};

#endif // SLOTHTXSOCKET_H
