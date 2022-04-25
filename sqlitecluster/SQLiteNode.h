#pragma once
#include <libstuff/libstuff.h>
#include <libstuff/SSynchronizedQueue.h>
#include <libstuff/STCPManager.h>
#include <sqlitecluster/SQLite.h>
#include <sqlitecluster/SQLitePool.h>
#include <sqlitecluster/SQLiteSequentialNotifier.h>
#include <WallClockTimer.h>
#include <SynchronizedMap.h>

// Convenience class for maintaining connections with a mesh of peers
#define PDEBUG(_MSG_) SDEBUG("->{" << peer->name << "} " << _MSG_)
#define PINFO(_MSG_) SINFO("->{" << peer->name << "} " << _MSG_)
#define PHMMM(_MSG_) SHMMM("->{" << peer->name << "} " << _MSG_)
#define PWARN(_MSG_) SWARN("->{" << peer->name << "} " << _MSG_)

// Diagnostic class for timing what fraction of time happens in certain blocks.
class AutoTimer {
  public:
    AutoTimer(string name) : _name(name), _intervalStart(chrono::steady_clock::now()), _countedTime(0) { }
    void start() { _instanceStart = chrono::steady_clock::now(); };
    void stop() {
        auto stopped = chrono::steady_clock::now();
        _countedTime += stopped - _instanceStart;
        if (stopped > (_intervalStart + 10s)) {
            auto counted = chrono::duration_cast<chrono::milliseconds>(_countedTime).count();
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(stopped - _intervalStart).count();
            static char percent[10] = {0};
            snprintf(percent, 10, "%.2f", static_cast<double>(counted) / static_cast<double>(elapsed) * 100.0);
            SINFO("[performance] AutoTimer (" << _name << "): " << counted << "/" << elapsed << " ms timed, " << percent << "%");
            _intervalStart = stopped;
            _countedTime = chrono::microseconds::zero();
        }
    };

  private:
    string _name;
    chrono::steady_clock::time_point _intervalStart;
    chrono::steady_clock::time_point _instanceStart;
    chrono::steady_clock::duration _countedTime;
};

class AutoTimerTime {
  public:
    AutoTimerTime(AutoTimer& t) : _t(t) { _t.start(); }
    ~AutoTimerTime() { _t.stop(); }

  private:
    AutoTimer& _t;
};

class SQLiteCommand;
class SQLiteServer;

// Distributed, leader/follower, failover, transactional DB cluster
class SQLiteNode : public STCPManager {
  public:

    // Possible states of a node in a DB cluster
    enum State {
        UNKNOWN,
        SEARCHING,     // Searching for peers
        SYNCHRONIZING, // Synchronizing with highest priority peer
        WAITING,       // Waiting for an opportunity to leader or follower
        STANDINGUP,    // Taking over leadership
        LEADING,       // Acting as leader node
        STANDINGDOWN,  // Giving up leader role
        SUBSCRIBING,   // Preparing to follow the leader
        FOLLOWING      // Following the leader node
    };

    // Represents a single peer in the database cluster
    class Peer {
      public:

        // Possible responses from a peer.
        enum class Response {
            NONE,
            APPROVE,
            DENY
        };

        // Const (and thus implicitly thread-safe) attributes of this Peer.
        const string name;
        const string host;
        const uint64_t id;
        const STable params;
        const bool permaFollower;

        // This is const because it's public, and we don't want it to be changed outside of this class, as it needs to
        // be synchronized with `hash`. However, it's often useful just as it is, so we expose it like this and update
        // it with `const_cast`. `hash` is only used in few places, so is private, and can only be accessed with
        // `getCommit`, thus reducing the risk of anyone getting out-of-sync commitCount and hash.
        const atomic<uint64_t> commitCount;

        // The rest of these are atomic so they can be read by multiple threads, but there's no special synchronization
        // required between them.
        atomic<int> failedConnections;
        atomic<uint64_t> latency;
        atomic<bool> loggedIn;
        atomic<uint64_t> nextReconnect;
        atomic<int> priority;
        atomic<State> state;
        atomic<Response> standupResponse;
        atomic<bool> subscribed;
        atomic<Response> transactionResponse;
        atomic<string> version;

        // An address on which this peer can accept commands.
        atomic<string> commandAddress;

        // Constructor.
        Peer(const string& name_, const string& host_, const STable& params_, uint64_t id_);
        ~Peer();

        // Atomically set commit and hash.
        void setCommit(uint64_t count, const string& hashString);

        // Atomically get commit and hash.
        void getCommit(uint64_t& count, string& hashString);

        // Gets an STable representation of this peer's current state in order to display status info.
        STable getData() const;

        // Returns true if there's an active connection to this Peer.
        bool connected() const;

        // Reset a peer, as if disconnected and starting the connection over.
        void reset();

        // Send a message to this peer. Thread-safe.
        void sendMessage(const SData& message);

        // Get a string name for a Response object.
        static string responseName(Response response);

      private:
        // The hash corresponding to commitCount.
        atomic<string> hash;

        // This allows direct access to the socket from the node object that should actually be managing peer
        // connections, which should always be handled by a single thread, and thus safe. Ideally, this isn't required,
        // but for the time being, the amount of refactoring required to fix that is too high.
        friend class SQLiteNode;
        Socket* socket = nullptr;

        // Mutex for locking around non-atomic member access (for set/getCommit, accessing socket, etc).
        mutable recursive_mutex _stateMutex;

        // For initializing the permafollower value from the params list.
        static bool isPermafollower(const STable& params);
    };

    // This exists to expose internal state to a test harness. It is not used otherwise.
    friend class SQLiteNodeTester;

    static const string& stateName(State state);
    static State stateFromName(const string& name);

    Socket* acceptSocket();

    // Do we need a mutex protecting this? Depends.
    list<STCPManager::Socket*> socketList;

    // Attributes
    string name;
    uint64_t recvTimeout;
    const vector<Peer*> peerList;
    list<Socket*> acceptedSocketList;

    // Receive timeout for 'normal' SQLiteNode messages
    static const uint64_t SQL_NODE_DEFAULT_RECV_TIMEOUT;

    // Separate timeout for receiving and applying synchronization commits.
    static const uint64_t SQL_NODE_SYNCHRONIZING_RECV_TIMEOUT;

    // Write consistencies available
    enum ConsistencyLevel {
        ASYNC,  // Fully asynchronous write, no follower approval required.
        ONE,    // Require exactly one approval (likely from a peer on the same LAN)
        QUORUM, // Require majority approval
        NUM_CONSISTENCY_LEVELS
    };
    static const string consistencyLevelNames[NUM_CONSISTENCY_LEVELS];

    // These are the possible states a transaction can be in.
    enum class CommitState {
        UNINITIALIZED,
        WAITING,
        COMMITTING,
        SUCCESS,
        FAILED
    };

    // Constructor/Destructor
    SQLiteNode(SQLiteServer& server, shared_ptr<SQLitePool> dbPool, const string& name, const string& host,
               const string& peerList, int priority, uint64_t firstTimeout, const string& version, const bool useParallelReplication = false,
               const string& commandPort = "localhost:8890");
    ~SQLiteNode();

    const vector<Peer*> initPeers(const string& peerList);

    // Simple Getters. See property definitions for details.
    State         getState()         { return _state; }
    int           getPriority()      { return _priority; }
    const string& getLeaderVersion() { return _leaderVersion; }
    const string& getVersion()       { return _version; }
    uint64_t      getCommitCount()   { return _db.getCommitCount(); }

    // Returns whether we're in the process of gracefully shutting down.
    bool gracefulShutdown() { return (_gracefulShutdownTimeout.alarmDuration != 0); }

    // True from when we call 'startCommit' until the commit has been sent to (and, if it required replication,
    // acknowledged by) peers.
    bool commitInProgress() { return (_commitState == CommitState::WAITING || _commitState == CommitState::COMMITTING); }

    // Returns true if the last commit was successful. If called while `commitInProgress` would return true, it returns
    // false.
    bool commitSucceeded() { return _commitState == CommitState::SUCCESS; }

    // Returns true if we're LEADING with enough FOLLOWERs to commit a quorum transaction. Not thread-safe to call
    // outside the sync thread.
    bool hasQuorum();

    // Call this if you want to shut down the node.
    void beginShutdown(uint64_t usToWait);

    // Prepare a set of sockets to wait for read/write.
    void prePoll(fd_map& fdm);

    // Handle any read/write events that occurred.
    void postPoll(fd_map& fdm, uint64_t& nextActivity);

    // Call this to check if the node's completed shutting down.
    bool shutdownComplete();

    // Updates the internal state machine. Returns true if it wants immediate re-updating. Returns false to indicate it
    // would be a good idea for the caller to read any new commands or traffic from the network.
    bool update();

    // Return the state of the lead peer. Returns UNKNOWN if there is no leader, or if we are the leader.
    State leaderState() const;

    // Begins the process of committing a transaction on this SQLiteNode's database. When this returns,
    // commitInProgress() will return true until the commit completes.
    void startCommit(ConsistencyLevel consistency);

    // If we have a command that can't be handled on a follower, we can escalate it to the leader node. The SQLiteNode
    // takes ownership of the command until it receives a response from the follower. When the command completes, it will
    // be re-queued in the SQLiteServer (_server), but its `complete` field will be set to true.
    // If the 'forget' flag is set, we will not expect a response from leader for this command.
    void escalateCommand(unique_ptr<SQLiteCommand>&& command, bool forget = false);

    // This takes a completed command and sends the response back to the originating peer. If we're not the leader
    // node, or if this command doesn't have an `initiatingPeerID`, then calling this function is an error.
    void sendResponse(const SQLiteCommand& command);

    // This is a static function that can 'peek' a command initiated by a peer, but can be called by any thread.
    // Importantly for thread safety, this cannot depend on the current state of the cluster or a specific node.
    // Returns false if the node can't peek the command.
    static bool peekPeerCommand(shared_ptr<SQLiteNode>, SQLite& db, SQLiteCommand& command);

    // This exists so that the _server can inspect internal state for diagnostic purposes.
    list<string> getEscalatedCommandRequestMethodLines();

    // This will broadcast a message to all peers, or a specific peer.
    void broadcast(const SData& message, Peer* peer = nullptr);

    // Tell the node a commit has been made by another thread, so that we can interrupt our poll loop if we're waiting
    // for data, and send the new commit.
    void notifyCommit();

    // Return the command address of the current leader, if there is one (empty string otherwise).
    string leaderCommandAddress() const;

  private:
    AutoTimer _deserializeTimer;
    AutoTimer _sConsumeFrontTimer;
    AutoTimer _sAppendTimer;

    // Returns a peer by it's ID. If the ID is invalid, returns nullptr.
    Peer* getPeerByID(uint64_t id);

    // Inverse of the above function. If the peer is not found, returns 0.
    uint64_t getIDByPeer(Peer* peer);

    unique_ptr<Port> port;

    // Called when we first establish a connection with a new peer
    void _onConnect(Peer* peer);

    // Called when we lose connection with a peer
    void _onDisconnect(Peer* peer);

    // Called when the peer sends us a message; throw an SException to reconnect.
    void _onMESSAGE(Peer* peer, const SData& message);

    // This is a pool of DB handles that this node can use for any DB access it needs. Currently, it hands them out to
    // replication threads as required. It's passed in via the constructor.
    shared_ptr<SQLitePool> _dbPool;

    // Handle to the underlying database that we write to. This should also be passed to an SQLiteCore object that can
    // actually perform some action on the DB. When those action are complete, you can call SQLiteNode::startCommit()
    // to commit and replicate them.
    SQLite& _db;

    // Choose the best peer to synchronize from. If no other peer is logged in, or no logged in peer has a higher
    // commitCount that we do, this will return null.
    void _updateSyncPeer();
    Peer* _syncPeer;

    // Store the ID of the last transaction that we replicated to peers. Whenever we do an update, we will try and send
    // any new committed transactions to peers, and update this value.
    static uint64_t _lastSentTransactionID;

    // Our priority, with respect to other nodes in the cluster. This is passed in to our constructor. The node with
    // the highest priority in the cluster will attempt to become the leader.
    atomic<int> _priority;

    // When the node starts, it is not ready to serve requests without first connecting to the other nodes, and checking
    // to make sure it's up-to-date. Store the configured priority here and use "-1" until we're ready to fully join the cluster.
    int _originalPriority;

    // Our current State.
    atomic<State> _state;
    
    // Pointer to the peer that is the leader. Null if we're the leader, or if we don't have a leader yet.
    atomic<Peer*> _leadPeer;

    // There's a mutex here to lock around changes to this, or any complex operations that expect leader to remain
    // unchanged throughout, notably, _sendToPeer. This is sort of a mess, but replication threads need to send
    // acknowledgments to the lead peer, but the main sync loop can update that at any time.
    mutable shared_mutex _leadPeerMutex;

    // Timestamp that, if we pass with no activity, we'll give up on our current state, and start over from SEARCHING.
    uint64_t _stateTimeout;

    // This is the current CommitState we're in with regard to committing a transaction. It is `UNINITIALIZED` from
    // startup until a transaction is started.
    CommitState _commitState;

    // The write consistency requested for the current in-progress commit.
    ConsistencyLevel _commitConsistency;

    // Stopwatch to track if we're going to give up on gracefully shutting down and force it.
    SStopwatch _gracefulShutdownTimeout;

    // Stopwatch to track if we're giving up on the server preventing a standdown.
    SStopwatch _standDownTimeOut;

    // Our version string. Supplied by constructor.
    string _version;

    // leader's version string.
    string _leaderVersion;

    // The maximum number of seconds we'll allow before we force a quorum commit. This can be violated when commits
    // are performed outside of SQLiteNode, but we'll catch up the next time we do a commit.
    int _quorumCheckpointSeconds;

    // The timestamp of the (end of) the last quorum commit.
    uint64_t _lastQuorumTime;

    // Helper methods
    void _sendToPeer(Peer* peer, const SData& message);
    void _sendToAllPeers(const SData& message, bool subscribedOnly = false);
    void _changeState(State newState);

    // Queue a SYNCHRONIZE message based on the current state of the node, thread-safe, but you need to pass the
    // *correct* DB for the thread that's making the call (i.e., you can't use the node's internal DB from a worker
    // thread with a different DB object) - which is why this is static.
    static void _queueSynchronize(SQLiteNode* node, Peer* peer, SQLite& db, SData& response, bool sendAll);
    void _recvSynchronize(Peer* peer, const SData& message);
    void _reconnectPeer(Peer* peer);
    void _reconnectAll();
    bool _isQueuedCommandMapEmpty();
    bool _isNothingBlockingShutdown();
    bool _majoritySubscribed();

    // When we're a follower, we can escalate a command to the leader. When we do so, we store that command in the
    // following map of commandID to Command until the follower responds.
    SynchronizedMap<string, unique_ptr<SQLiteCommand>> _escalatedCommandMap;

    // Replicates any transactions that have been made on our database by other threads to peers.
    void _sendOutstandingTransactions(const set<uint64_t>& commitOnlyIDs = {});

    // The server object to which we'll pass incoming escalated commands.
    SQLiteServer& _server;

    // This is an integer that increments every time we change states. This is useful for responses to state changes
    // (i.e., approving standup) to verify that the messages we're receiving are relevant to the current state change,
    // and not stale responses to old changes.
    int _stateChangeCount;

    // Last time we recorded network stats.
    chrono::steady_clock::time_point _lastNetStatTime;

    // Handler for transaction messages.
    void handleBeginTransaction(SQLite& db, Peer* peer, const SData& message, bool wasConflict);
    void handlePrepareTransaction(SQLite& db, Peer* peer, const SData& message);
    int handleCommitTransaction(SQLite& db, Peer* peer, const uint64_t commandCommitCount, const string& commandCommitHash);
    void handleRollbackTransaction(SQLite& db, Peer* peer, const SData& message);
    
    // Legacy versions of the above functions for serial replication.
    void handleSerialBeginTransaction(Peer* peer, const SData& message);
    void handleSerialCommitTransaction(Peer* peer, const SData& message);
    void handleSerialRollbackTransaction(Peer* peer, const SData& message);

    WallClockTimer _syncTimer;
    atomic<uint64_t> _handledCommitCount;

    // State variable that indicates when the above threads should quit.
    atomic<bool> _replicationThreadsShouldExit;

    SQLiteSequentialNotifier _localCommitNotifier;
    SQLiteSequentialNotifier _leaderCommitNotifier;

    // This is the main replication loop that's run in the replication threads. It's instantiated in a new thread for
    // each new relevant replication command received by the sync thread.
    //
    // There are three commands we currently handle here BEGIN_TRANSACTION, ROLLBACK_TRANSACTION, and
    // COMMIT_TRANSACTION.
    // ROLLBACK_TRANSACTION and COMMIT_TRANSACTION are trivial, they record the new highest commit number from LEADER,
    // or instruct the node to go SEARCHING and reconnect if a distributed ROLLBACK happens.
    //
    // BEGIN_TRANSACTION is where the interesting case is. This starts all transactions in parallel, and then waits
    // until each previous transaction is committed such that the final commit order matches LEADER. It also handles
    // commit conflicts by re-running the transaction from the beginning. Most of the logic for making sure
    // transactions are ordered correctly is done in `SQLiteSequentialNotifier`, which is worth reading.
    //
    // This thread exits on completion of handling the command or when node._replicationThreadsShouldExit is set,
    // which happens when a node stops FOLLOWING.
    static void replicate(SQLiteNode& node, Peer* peer, SData command, size_t sqlitePoolIndex);

    // Counter of the total number of currently active replication threads. This is used to let us know when all
    // threads have finished.
    atomic<int64_t> _replicationThreadCount;

    // Indicates whether this node is configured for parallel replication.
    const bool _useParallelReplication;

    // Monotonically increasing thread counter, used for thread IDs for logging purposes.
    static atomic<int64_t> _currentCommandThreadID;

    // Utility class that can decrement _replicationThreadCount when objects go out of scope.
    template <typename CounterType>
    class ScopedDecrement {
      public:
        ScopedDecrement(CounterType& counter) : _counter(counter) {} 
        ~ScopedDecrement() {
            --_counter;
        }
      private:
        CounterType& _counter;
    };

    AutoTimer _multiReplicationThreadSpawn;
    AutoTimer _legacyReplication;
    AutoTimer _onMessageTimer;
    AutoTimer _escalateTimer;

    // A string representing an address (i.e., `127.0.0.1:80`) where this server accepts commands. I.e., "the command
    // port".
    const string _commandAddress;

    // This is just here to allow `poll` to get interrupted when there are new commits to send. We don't want followers
    // to wait up to a full second for them.
    SSynchronizedQueue<bool> _commitsToSend;

    // Override dead function
    void postPoll(fd_map& ignore) { SERROR("Don't call."); }

    // Helper functions
    void _sendPING(Peer* peer);
};

// serialization for Responses.
ostream& operator<<(ostream& os, const atomic<SQLiteNode::Peer::Response>& response);

