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

    txSocket.initiateHandshake("somefile", 50, "127.0.0.1", 5000);

    return a.exec();
}
