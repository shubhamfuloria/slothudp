#include <QCoreApplication>
#include <QDebug>
#include <QThread>
#include <QFileInfo>
#include <QDir>

#include <include/slothrxsocket.h>
#include <include/slothtxsocket.h>

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    qInfo() << "Hello I'm Sloth UDP, a reliable file transfer protocol";

    // Initialize sockets
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

    // Parse command line arguments
    QString destinationIP;
    QString fileName;

    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);

        if (arg == "-d" && i + 1 < argc) {
            destinationIP = QString::fromLocal8Bit(argv[i + 1]);
            i++; // Skip next argument as it's the IP value
        }
        else if (arg == "-f" && i + 1 < argc) {
            fileName = QString::fromLocal8Bit(argv[i + 1]);
            i++; // Skip next argument as it's the filename value
        }
    }

    // Check if both destination IP and filename are provided
    if (!destinationIP.isEmpty() && !fileName.isEmpty()) {
        // Determine file path
        QString filePath;
        QFileInfo fileInfo(fileName);

        if (fileInfo.isAbsolute()) {
            // Full path provided
            filePath = fileName;
        } else {
            // Relative path - file should be in same directory
            filePath = QDir::currentPath() + "/" + fileName;
        }

        // Check if file exists
        QFile file(filePath);
        if (!file.exists()) {
            qCritical() << "File does not exist:" << filePath;
            return -1;
        }

        qint64 fileSize = file.size();

        // Send handshake packet
        QMetaObject::invokeMethod(tx, [=]() {
                qDebug() << "File size is:" << fileSize;
                tx->initiateHandshake(filePath, fileSize, destinationIP, 5000);
            }, Qt::QueuedConnection);

        qInfo() << "Initiating file transfer to" << destinationIP << "with file:" << filePath;
    }
    else if (!destinationIP.isEmpty() || !fileName.isEmpty()) {
        // Only one argument provided
        qWarning() << "Both destination IP (-d) and filename (-f) must be provided for file transfer";
        qInfo() << "Usage: program -d <destination_ip> -f <filename>";
        qInfo() << "Sockets initialized without file transfer";
    }
    else {
        // No arguments provided - just initialize sockets
        qInfo() << "No arguments provided. Sockets initialized and ready.";
    }

    return a.exec();
}
