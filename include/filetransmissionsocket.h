#ifndef FILETRANSMISSIONSOCKET_H
#define FILETRANSMISSIONSOCKET_H

#include <QObject>
#include <QUdpSocket>

class FileTransmissionSocket : QUdpSocket
{
public:
    FileTransmissionSocket();

private slots:
    void rxUdpSocket();
};

#endif // FILETRANSMISSIONSOCKET_H
