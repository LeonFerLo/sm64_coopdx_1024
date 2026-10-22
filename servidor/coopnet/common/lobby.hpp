#pragma once

#include <vector>
#include <mutex>
#include "socket.hpp"
#include "connection.hpp"
#include "utils.hpp"

class Connection;

class Lobby {
    private:
    public:
        bool mActive = false;
        Connection* mOwner = nullptr;
        uint64_t mId = 0;
        std::vector<Connection*> mConnections;
        uint16_t mMaxConnections = 16;
        uint32_t mNextPriority = 0;

        /* partida masiva (más de 16): los jugadores entran por smnet; el host se registra por
           UDP (rendezvous.cpp) y avisa cuántos hay */
        bool mMassive = false;
        uint8_t mSecret[16] = { 0 };
        uint16_t mReportedCount = 1;
        uint32_t mHostIp = 0;       /* dirección pública del socket de smnet del host (orden de red) */
        uint16_t mHostPort = 0;     /* (orden de red) */
        uint64_t mHostSeen = 0;     /* segundos */

        std::string mGame;
        std::string mVersion;
        std::string mHostName;
        std::string mMode;
        std::string mPassword;
        std::string mDescription;

        Lobby(Connection* aOwner, uint64_t aId, std::string& aGame, std::string& aVersion, std::string& aHostName, std::string& aMode, uint16_t aMaxConnections, std::string& aPassword, std::string& aDescription);
        ~Lobby();

        enum MPacketErrorNumber Join(Connection* aConnection, std::string& aPassword);
        void Leave(Connection* aConnection);
};

// callbacks
extern void (*gOnLobbyJoin)(Lobby* lobby, Connection* connection);
extern void (*gOnLobbyLeave)(Lobby* lobby, Connection* connection);
extern void (*gOnLobbyDestroy)(Lobby* lobby);
