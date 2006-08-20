/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana World is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana World; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  $Id$
 */

#include "gamehandler.h"

#include <iostream>
#include <map>

#include "gameclient.h"
#include "map.h"
#include "messagein.h"
#include "messageout.h"
#include "netcomputer.h"
#include "state.h"
#include "utils/logger.h"

struct GamePendingLogin
{
    PlayerPtr character;
    int timeout;
};

typedef std::map< std::string, GamePendingLogin > GamePendingLogins;
typedef std::map< std::string, GameClient * > GamePendingClients;

/**
 * The pending logins represent clients who were given a magic token by the
 * account server but who have not yet logged in to the game server.
 */
static GamePendingLogins pendingLogins;

/**
 * The pending clients represent clients who tried to login to the game server,
 * but for which no magic token is available yet. This can happen when the
 * communication between the account server and client went faster than the
 * communication between the account server and the game server.
 */
static GamePendingClients pendingClients;

/**
 * Notification that a particular token has been given to allow a certain
 * player to enter the game.
 *
 * This method is currently called directly from the account server. Later on
 * it should be a message sent from the account server to the game server.
 */
void registerGameClient(std::string const &token, PlayerPtr ch)
{
    GamePendingClients::iterator i = pendingClients.find(token);
    if (i != pendingClients.end())
    {
        GameClient *computer = i->second;
        computer->setCharacter(ch);
        pendingClients.erase(i);
        MessageOut result;
        result.writeShort(GPMSG_CONNECT_RESPONSE);
        result.writeByte(ERRMSG_OK);
        computer->send(result);
    }
    else
    {
        GamePendingLogin p;
        p.character = ch;
        p.timeout = 300; // world ticks
        pendingLogins.insert(std::make_pair(token, p));
    }
}

void GameHandler::removeOutdatedPending()
{
    GamePendingLogins::iterator i = pendingLogins.begin();
    GamePendingLogins::iterator next;

    while (i != pendingLogins.end())
    {
        next = i;
        ++next;
        if (--i->second.timeout <= 0)
        {
            pendingLogins.erase(i);
        }
        i = next;
    }
}

NetComputer *GameHandler::computerConnected(ENetPeer *peer)
{
    return new GameClient(this, peer);
}

void GameHandler::computerDisconnected(NetComputer *computer)
{
    GamePendingClients::iterator i;
    for (i = pendingClients.begin(); i != pendingClients.end(); ++i)
    {
        if (i->second == computer)
        {
            pendingClients.erase(i);
            break;
        }
    }
    delete computer;
}

void GameHandler::process()
{
    ConnectionHandler::process();
    removeOutdatedPending();
}

void GameHandler::processMessage(NetComputer *comp, MessageIn &message)
{
    GameClient &computer = *static_cast< GameClient * >(comp);
    MessageOut result;

    if (computer.getCharacter().get() == NULL) {
        if (message.getId() != PGMSG_CONNECT) return;
        std::string magic_token = message.readString(32);
        GamePendingLogins::iterator i = pendingLogins.find(magic_token);
        if (i == pendingLogins.end())
        {
            GamePendingClients::iterator i;
            for (i = pendingClients.begin(); i != pendingClients.end(); ++i)
            {
                if (i->second == &computer) return;
            }
            pendingClients.insert(std::make_pair(magic_token, &computer));
            return;
        }
        computer.setCharacter(i->second.character);
        pendingLogins.erase(i);
        result.writeShort(GPMSG_CONNECT_RESPONSE);
        result.writeByte(ERRMSG_OK);
        computer.send(result);
        return;
    }

    switch (message.getId())
    {
        case PGMSG_SAY:
            {
                std::string say = message.readString();
                sayAround(computer, say);
            } break;

        case PGMSG_PICKUP:
            {
                // add item to inventory (this is too simplistic atm)
                unsigned int itemId = message.readLong();

                // remove the item from world map

                // send feedback
                computer.getCharacter()->addInventory(itemId);
                result.writeShort(GPMSG_PICKUP_RESPONSE);
                result.writeByte(ERRMSG_OK);
            } break;

        case PGMSG_USE_ITEM:
            {
                unsigned int itemId = message.readLong();

                result.writeShort(GPMSG_USE_RESPONSE);

                if (computer.getCharacter()->hasItem(itemId)) {
                    // use item
                    // this should execute a script which will do the appropriate action
                    // (the script will determine if the item is 1 use only)
                    result.writeByte(ERRMSG_OK);
                } else {
                    result.writeByte(ERRMSG_FAILURE);
                }
            } break;

        case PGMSG_WALK:
            {
                unsigned x = message.readShort();
                unsigned y = message.readShort();

                computer.getCharacter()->setDestination(x, y);

                // no response should be required
            } break;

        case PGMSG_EQUIP:
            {
                int itemId = message.readLong();
                char slot = message.readByte();

                result.writeShort(GPMSG_EQUIP_RESPONSE);
                result.writeByte(computer.getCharacter()->equip(itemId, slot) ?
                                 ERRMSG_OK : ERRMSG_FAILURE);
            } break;

        default:
            LOG_WARN("Invalid message type", 0);
            result.writeShort(XXMSG_INVALID);
            break;
    }

    if (result.getDataSize() > 0)
        computer.send(result);
}

void GameHandler::sayAround(GameClient &computer, std::string const &text)
{
    PlayerPtr beingPtr = computer.getCharacter();

    MessageOut msg(GPMSG_SAY);
    msg.writeString(beingPtr->getName());
    msg.writeString(text);

    unsigned speakerMapId = beingPtr->getMapId();
    std::pair<unsigned, unsigned> speakerXY = beingPtr->getXY();

    for (NetComputers::iterator i = clients.begin(), i_end = clients.end();
         i != i_end; ++i)
    {
        // See if the other being is near enough, then send the message
        Player const *listener =
            static_cast<GameClient *>(*i)->getCharacter().get();

        if (!listener || listener->getMapId() != speakerMapId) {
            continue;
        }

        std::pair<unsigned, unsigned> listenerXY = listener->getXY();

        if (areAround(listenerXY.first, listenerXY.second,
                      speakerXY.first, speakerXY.second))
        {
            (*i)->send(msg);
        }
    }
}

void
GameHandler::sendTo(PlayerPtr beingPtr, MessageOut &msg)
{
    /* TODO: This implementation is very inefficient. An alternative would be
     * store the NetComputer reference with the player class, so that it can
     * be directly accessed.
     */
    for (NetComputers::iterator i = clients.begin(); i != clients.end(); ++i)
    {
        PlayerPtr clientChar = static_cast<GameClient *>(*i)->getCharacter();
        if (clientChar.get() == beingPtr.get())
        {
            (*i)->send(msg);
            break;
        }
    }
}
