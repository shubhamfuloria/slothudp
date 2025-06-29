#include <QCoreApplication>
#include <QDebug>
#include <QThread>

#include <include/slothrxsocket.h>
#include <include/slothtxsocket.h>

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qInfo() << "Hello I'm Sloth UDP, a reliable file transfer protocol";

    SlothRxSocket* rx = new SlothRxSocket(); // lives on heap
    QThread* rxThread = new QThread();       // also on heap

    rx->moveToThread(rxThread);

    QObject::connect(rxThread, &QThread::finished, rx, &QObject::deleteLater);
    QObject::connect(rxThread, &QThread::finished, rxThread, &QObject::deleteLater);

    rxThread->start();



    SlothTxSocket* tx = new SlothTxSocket();
    QThread* txThread = new QThread();
    tx->moveToThread(txThread);
    QObject::connect(txThread, &QThread::finished, tx, &QObject::deleteLater);
    txThread->start();

    QMetaObject::invokeMethod(tx, [=]() {
            QString filePath = "C:/Users/Shubham/Videos/test.jpg";
            QFile file(filePath);
            qDebug() << "size is: " << file.size();
            tx->initiateHandshake(filePath, file.size(), "127.0.0.1", 5000);
        }, Qt::QueuedConnection);

    return a.exec();
}
