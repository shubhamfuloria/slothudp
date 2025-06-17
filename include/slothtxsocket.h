#ifndef SLOTHTXSOCKET_H
#define SLOTHTXSOCKET_H

#include <QObject>
#include <QUdpSocket>

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
    void handleHandshakeAck(const QByteArray& packet);

    DataPacket createDataPacket(int seqNum, const QByteArray& chunk);
    QByteArray serializePacket(const DataPacket& packet);

    QByteArray serializePacket(const HandshakePacket& packet);


    void sendWindow();
    void sendPacket(DataPacket packet);

    QByteArray readChunkFromFile();
    void resetFile();

    /**
     * @brief sendPacket: sends the buffer to network m_desitnation address
     * @param buffer
     * @return true on success
     */
    bool transmitBuffer(const QByteArray& buffer);


    void stopTransmission();

    QString m_filePath;
    QString m_fileSize;
    QHostAddress m_destAddress;
    quint16 m_destPort;
    int m_windowSize;
    int m_base;



private slots:
    /**
     * @brief handleReadyRead: gets triggered when the socket receives some datagram
     */
    void handleReadyRead();



};

#endif // SLOTHTXSOCKET_H
