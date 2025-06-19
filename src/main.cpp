#include <QCoreApplication>
#include <QDebug>

#include <include/slothrxsocket.h>
#include <include/slothtxsocket.h>

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qInfo() << "Hello I'm Sloth UDP, a reliable file transfer protocol";

    SlothTxSocket txSocket;
    SlothRxSocket rxSocket;

    txSocket.initiateHandshake("C:/Users/Shubham/Videos/test.png", 50, "172.16.6.29", 5000);

    return a.exec();
}
