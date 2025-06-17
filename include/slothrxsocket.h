#ifndef SLOTHRXSOCKET_H
#define SLOTHRXSOCKET_H

#include <QObject>
#include <QUdpSocket>

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
     * @brief transmitBuffer: Writes the buffer to the network to m_txAddress
     * @param buffer
     */
    bool transmitBuffer(const QByteArray& buffer);


    QHostAddress m_txAddress;
    quint16 m_txPort;

private slots:
    void handleReadyRead();

signals:
    void on_fileTxRequest(QString fileName, quint64 fileSize, QString hostAddress);
};

#endif // SLOTHTXSOCKET_H
