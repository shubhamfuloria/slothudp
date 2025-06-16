#ifndef SLOTHTXSOCKET_H
#define SLOTHTXSOCKET_H

#include <QObject>
#include <QUdpSocket>

#include <include/types.h>

class SlothTxSocket : QUdpSocket
{
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


    void stopTransmission();

    QString m_filePath;
    QString m_fileSize;
    QHostAddress m_destAddress;
    quint16 m_destPort;



private slots:
    void rxUdpSocket();



};

#endif // SLOTHTXSOCKET_H
