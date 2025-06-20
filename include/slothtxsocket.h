#ifndef SLOTHTXSOCKET_H
#define SLOTHTXSOCKET_H

#include <QObject>
#include <QUdpSocket>
#include <QFile>

#include <include/types.h>

class SlothTxSocket : public QUdpSocket
{
    Q_OBJECT

public:
    SlothTxSocket();

    /**
     * @brief initiateHandshake: The api exposed publically, application using the sloth library can call this
     *                           function to initiate handshake
     * @param filePath
     * @param fileSize
     * @param destination
     * @param port
     */
    void initiateHandshake(const QString& filePath, qint64 fileSize, QString destination, quint16 port);

private:
    void handleHandshakeAck(quint32 requestId);
    void handleDataAck(PacketHeader header, QByteArray payload);

    DataPacket createDataPacket(int seqNum, const QByteArray& chunk);


    void sendNextWindow();

    void sendWindow();
    void sendPacket(DataPacket packet);
    void sendEOFPacket();


    QByteArray readChunkFromFile();
    void resetFile();

    /**
     * @brief sendPacket: sends the buffer to network m_desitnation address
     * @param buffer
     * @return true on success
     */
    bool transmitBuffer(const QByteArray& buffer);

    /**
     * @brief initiateFileTransfer: initializes the file present at m_filePath and prepares it for file transfer
     * @return
     */
    bool initiateFileTransfer();


    void stopTransmission();
    void handleBye();

    QString m_filePath;
    QString m_fileSize;
    QFile m_file;
    QHostAddress m_destAddress;
    quint16 m_destPort;
    int m_windowSize;

    /**
     * @brief m_baseSeqNum: stores oldest unacknowledged packet
     */
    int m_baseSeqNum;

    /**
     * @brief m_nextSeqNum: points to next packet to send
     * PS: we won't wait for all packets to be acked, we'll keep sending new packets
     */
    int m_nextSeqNum;
    int m_chunkSize = 700;
    quint8 m_protoVer = 1;
    QMap<quint32, QByteArray>m_sendWindow;
    QMap<quint32, QByteArray>m_inFlightWindow;



private slots:
    /**
     * @brief handleReadyRead: gets triggered when the socket receives some datagram
     */
    void handleReadyRead();



};

#endif // SLOTHTXSOCKET_H
