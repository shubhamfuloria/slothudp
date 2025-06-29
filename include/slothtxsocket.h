#ifndef SLOTHTXSOCKET_H
#define SLOTHTXSOCKET_H

#include <QObject>
#include <QUdpSocket>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QQueue>

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
    void handleHandshakeAck(quint32 requestId);
    void handleDataAck(PacketHeader header, QByteArray payload);
    void handleNack(PacketHeader header, QByteArray buffer);

    DataPacket createDataPacket(int seqNum, const QByteArray& chunk);


    void sendNextWindow();

    void sendWindow();
    void sendPacket(DataPacket& packet);
    void sendEOFPacket();


    QByteArray readChunkFromFile();
    void resetFile();

    /**
     * @brief sendPacket: sends the buffer to network m_desitnation address
     * @param buffer
     * @return true on success
     */
    bool transmitBuffer(const QByteArray& buffer);

    /**
     * @brief initiateFileTransfer: initializes the file present at m_filePath and prepares it for file transfer
     * @return
     */
    bool initiateFileTransfer();


    void stopTransmission();
    void handleBye();

    QString m_filePath;
    QString m_fileSize;
    QFile m_file;
    QHostAddress m_destAddress;
    quint16 m_destPort;
    int m_windowSize;

    /**
     * @brief m_baseSeqNum: stores oldest unacknowledged packet
     */
    int m_baseSeqNum;

    /**
     * @brief m_nextSeqNum: points to next packet to send
     * PS: we won't wait for all packets to be acked, we'll keep sending new packets
     */
    int m_nextSeqNum;
    int m_chunkSize = 1024;
    quint8 m_protoVer = 1;
    QElapsedTimer* m_rttTimer = nullptr;
    QHash<quint32, quint64> m_sentTimestamp; // seql -> milisecond, to calculate RTT
    double m_estimatedRTT = 300.0;
    quint64 m_devRTT = 0;
    quint64 m_RTO = 1000; // retransmission timeout
    QTimer* m_retransmitTimer = nullptr;


    QHash<quint32, QByteArray>m_sendWindow;
    QHash<quint32, QByteArray>m_inFlightWindow;
    QSet<quint32> m_missingWindow; // contains nack ids received from client
    QHash<quint32, quint64> m_lastRextTime;

    quint32 m_activeSessionId;
    SessionState m_sessionState = SessionState::NOTACTIVE;

    /**
     * @brief m_handshakeReqRetryCount: sender keeps trying to send handshake packet to client,
     * to handle the packet loss of the handshake packet.
     * server will try this many times before giving up
     */
    int m_handshakeReqRetryLimit = 5;
    int m_handshakeReqRetryCount = 0;

    QTimer* m_handshakeRetryTimer = nullptr;

    // Adaptive Window Control
    quint32 m_congestionWindow = 10;      // Current congestion window
    quint32 m_slowStartThreshold = 65535; // Slow start threshold
    quint32 m_maxWindow = 100;            // Maximum allowed window
    quint32 m_minWindow = 4;              // Minimum window size

    // Performance monitoring
    quint64 m_lastWindowAdjustment = 0;   // Time of last window change
    quint32 m_consecutiveGoodAcks = 0;    // Count of ACKs without loss
    quint32 m_recentLossCount = 0;        // Recent loss events
    quint64 m_lastLossTime = 0;           // Time of last loss

    // Bandwidth estimation
    quint64 m_bytesAcked = 0;             // Bytes acknowledged
    quint64 m_lastBandwidthCalc = 0;      // Last bandwidth calculation time
    double m_estimatedBandwidth = 0;      // Estimated bandwidth (bytes/ms)

    // Round-trip time tracking
    QQueue<quint64> m_rttSamples;         // Recent RTT samples
    quint64 m_minRTT = UINT64_MAX;        // Minimum observed RTT

    // Duplicate ACK detection
    QHash<quint32, quint32> m_duplicateAckCount;
    quint32 m_lastAckSeq = 0;

    quint32 getEffectiveWindowSize();
    void adaptMaxWindow();
    void updateBandwidthEstimate(quint64 now);
    void handleLossEvent(quint64 now);
    void updateRTTAndBandwidth(quint32 seq, quint64 now);
    void handleSuccessfulAck(quint32 newlyAckedPackets, quint64 now);

private slots:
    /**
     * @brief handleReadyRead: gets triggered when the socket receives some datagram
     */
    void handleReadyRead();

    void handleRetransmissions();



};

#endif // SLOTHTXSOCKET_H
