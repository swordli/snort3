//--------------------------------------------------------------------------
// Copyright (C) 2015-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// ha.h author Ed Borgoyn <eborgoyn@cisco.com>

#ifndef HA_H
#define HA_H

#include "main/snort_types.h"
#include "packet_io/sfdaq.h"
#include "side_channel/side_channel.h"

//-------------------------------------------------------------------------

class Flow;

// The FlowHAHandle is the dynamically allocated index used uniquely identify
//   the client.  Used both in the API and HA messages.
// Handle 0 is defined to be the primary session client.
// NOTE: The type, masks, and count values must be in sync,
typedef uint16_t FlowHAClientHandle;
const FlowHAClientHandle SESSION_HA_CLIENT = 0x0000;
const uint8_t SESSION_HA_CLIENT_INDEX = 0;
const FlowHAClientHandle ALL_CLIENTS = 0xffff;
// One client for each mask bit plus one 'automatic' session client
//   client handle = (1<<(client_index-1)
//   session client has handle of 0 and index of 0
const uint8_t MAX_CLIENTS = 17;

enum HAEvent
{
    HA_DELETE_EVENT = 1,
    HA_UPDATE_EVENT = 2
};

// Each active flow will have an associated FlowHAState instance.
class FlowHAState
{
public:
    static const uint8_t CRITICAL = 0x20;
    static const uint8_t MAJOR = 0x10;

    static const uint8_t CREATED = 0x01;
    static const uint8_t MODIFIED = 0x02;
    static const uint8_t DELETED = 0x04;
    static const uint8_t STANDBY = 0x08;

    FlowHAState();
    ~FlowHAState() {}

    void set_pending(FlowHAClientHandle);
    void clear_pending(FlowHAClientHandle);
    bool check_pending(FlowHAClientHandle);
    void set(uint8_t state);
    void set(uint8_t state, uint8_t priority);
    void clear(uint8_t state);
    void clear(uint8_t state, uint8_t priority);
    bool check(uint8_t state);
    bool is_critical();
    bool is_major();
    static void config_lifetime(timeval);
    bool old_enough();
    void set_next_update();
    void initialize_update_time();
    void reset();

private:
    static const uint8_t INITIAL_STATE = 0x00;
    static const uint16_t NONE_PENDING = 0x0000;
    static const uint8_t PRIORITY_MASK = 0x30;
    static const uint8_t STATUS_MASK = 0x0f;

    static struct timeval min_session_lifetime;
    uint8_t state;
    uint16_t pending;
    struct timeval next_update;
};

struct __attribute__((__packed__)) HAMessageHeader
{
    uint8_t event;
    uint8_t version;
    uint16_t total_length;
    uint8_t key_type;
};

struct __attribute__((__packed__)) HAClientHeader
{
    uint8_t client;
    uint8_t length;
};

// Describe the message being produced or consumed.
class HAMessage
{
public:
    HAMessage(SCMessage* msg)
    { sc_msg = msg; }
    ~HAMessage() { }

    uint8_t* content()
    { return sc_msg->content; }
    uint16_t content_length()
    { return sc_msg->content_length; }
    uint8_t* cursor;

private:
    SCMessage* sc_msg;
};

// A FlowHAClient subclass for each producer/consumer of flow HA data
class FlowHAClient
{
public:
    virtual ~FlowHAClient() { }
    virtual bool consume(Flow*, HAMessage*) { return false; }
    virtual bool produce(Flow*, HAMessage*) { return false; }
    uint8_t get_message_size() { return header.length; }
    bool fit(HAMessage*, uint8_t);
    bool place(HAMessage*, uint8_t*, uint8_t);
    FlowHAClientHandle handle;  // Actual handle for the instance
    HAClientHeader header;

protected:
    FlowHAClient(uint8_t, bool);

};

// HighAvailability is instantiated for each packet-thread.
// FIXIT-M make the SideChannel the THREAD_LOCAL element and collapse
//  into HighAvailabilityManager
class HighAvailability
{
public:
    HighAvailability(PortBitSet*,bool);
    ~HighAvailability();

    void process_update(Flow*, const DAQ_PktHdr_t*);
    void process_deletion(Flow*);
    void process_receive();

private:
    void receive_handler(SCMessage*);
    SideChannel* sc = nullptr;
};

// Top level management of HighAvailability components.
class HighAvailabilityManager
{
public:
    // Prior to parsing configuration
    static void pre_config_init();
    // Invoked by the module configuration parsing to create HA instance
    static bool instantiate(PortBitSet*,bool);
    static void thread_init();
    static void thread_term();
    // true is we are configured and able to process
    static bool active();
    // Within the packet callback, analyze the packet and flow for potential update messages
    static void process_update(Flow*, const DAQ_PktHdr_t*);
    // Anytime a flow is deleted, potentially generate a deletion message
    static void process_deletion(Flow*);
    // Look for and dispatch receive messages.
    static void process_receive();

private:
    HighAvailabilityManager() = delete;
    static bool use_daq_channel;
    static PortBitSet* ports;
};
#endif

