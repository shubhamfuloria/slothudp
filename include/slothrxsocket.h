#ifndef SLOTHRXSOCKET_H
#define SLOTHRXSOCKET_H

#include <QObject>
#include <QUdpSocket>

class SlothRxSocket : public QUdpSocket
{
    Q_OBJECT

public:

    SlothRxSocket();

private slots:
    void handleReadyRead();
};

#endif // SLOTHTXSOCKET_H
