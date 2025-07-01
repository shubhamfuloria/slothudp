#ifndef SLOTHRXSOCKET_H
#define SLOTHRXSOCKET_H

#include <QObject>
#include <QUdpSocket>
#include <QFile>
#include <QTimer>
#include <QTime>

#include <include/types.h>

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
     * @brief handleHandshakePacket
     * @param buffer: buffer without packet header, HandshakePacket
     */
    void handleHandshakePacket(QByteArray buffer);


    void handlePacket(PacketHeader header, QByteArray payload);

    /**
     * @brief transmitBuffer: Writes the buffer to the network to m_txAddress
     * @param buffer
     */
    bool transmitBuffer(const QByteArray& buffer);


    void sendAcknowledgement();

    /**
     * @brief generateAckBitmap: Creates a ack bitmap starting from base. eg. if packet 3, 4, 6, 7 has been received
     *                           base will be 3 and bitmap will be 11011 ( 27 )
     * @param base
     * @param windowSize
     * @return
     */
    QByteArray generateAckBitmap(quint32 base, int windowSize);
    void sayByeToPeer(); // and tear down connections
    void scheduleNackDebounce();
    void performNackDebounce();
    void sendNack(QList<quint32> missing);

    void startFeedbackTimers();

    QHostAddress m_txAddress;
    quint16 m_txPort;
    uint m_windowSize = 8;


    QString m_filePath;
    QFile m_file;

    int m_baseAckSeqNum;
    int m_baseWriteSeqNum;

    /**
     * @brief m_untrackedCount represents packet which are received, but we haven't sent acknowledgment for them yet.
     */
    int m_untrackedCount;
    QHash<quint32, QByteArray>m_recvWindow;
    QSet<quint32> m_receivedSeqNums;


    QSet<quint32> m_pendingMissing;

    QTimer* m_nackTimer = nullptr;
    QTimer* m_feedbackTimer = nullptr;
    QTime m_lastAckTime;
    QTime m_lastNackTime;
    quint32 m_highestSeqReceived = 0;
    bool m_nackDebounceScheduled = false;

    quint32 m_activeSessionId = 0;
    SessionState m_sessionState = SessionState::NOTACTIVE;

    // Progress tracking
    QTimer* m_progressTimer = nullptr;
    QTime m_progressStartTime;
    quint64 m_expectedFileSize = 0;

    bool m_transferCompleted = false;
    QTimer* m_byeConfirmTimer = nullptr;
    void handleFinPacket(PacketHeader header);
    void sendByeConfirmation();
    void performReceiveCleanup(bool successful);

    // Statistics structure
    struct RxStats {
        quint64 totalBytesReceived = 0;
        quint64 uniqueBytesReceived = 0;
        quint32 totalPacketsReceived = 0;
        quint32 duplicatePacketsReceived = 0;
        quint32 totalPacketsLost = 0;
        quint32 outOfOrderPackets = 0;
    } m_stats;

private slots:
    void handleReadyRead();
    void handleNackTimeout();
    void handleFeedbackTimeout();
    void startProgressTimer();
    void stopProgressTimer();
    void printProgress();
    void printFinalStats();

signals:
    void on_fileTxRequest(QString fileName, quint64 fileSize, QString hostAddress);
    void on_byeTimerFinished();
    void transferCompleted(bool, QString, quint64, quint64);


};

#endif // SLOTHTXSOCKET_H
