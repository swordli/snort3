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

// tcp_segment_descriptor.cc author davis mcpherson <davmcphe@cisco.com>
// Created on: Jul 30, 2015

#include "log/messages.h"
#include "main/snort_debug.h"
#include "protocols/tcp_options.h"
#include "detection/rules.h"

#include "stream/tcp/tcp_defs.h"
#include "stream/tcp/tcp_event_logger.h"
#include "tcp_segment_descriptor.h"

using namespace tcp;

TcpSegmentDescriptor::TcpSegmentDescriptor(Flow* flow, Packet* pkt, TcpEventLogger& tel) :
    flow(flow), pkt(pkt)
{
    tcph = pkt->ptrs.tcph;
    src_port = tcph->src_port();
    dst_port = tcph->dst_port();
    seg_seq = tcph->seq();
    seg_ack = tcph->ack();
    seg_wnd = tcph->win();
    end_seq = seg_seq + (uint32_t)pkt->dsize;
    ts = 0;

    // don't bump end_seq for fin here we will bump if/when fin is processed
    if ( tcph->is_syn() )
    {
        end_seq++;
        if ( !tcph->is_ack() )
            tel.log_internal_event(INTERNAL_EVENT_SYN_RECEIVED);
    }

    #ifdef DEBUG_STREAM_EX
    print_tsd( );
    #endif
}

TcpSegmentDescriptor::~TcpSegmentDescriptor()
{
    // TODO Auto-generated destructor stub
}

uint32_t TcpSegmentDescriptor::init_mss(uint16_t* value)
{
    DebugMessage(DEBUG_STREAM_STATE, "Getting MSS...\n");

    TcpOptIterator iter(tcph, pkt);
    for ( const TcpOption& opt : iter )
    {
        if ( opt.code == TcpOptCode::MAXSEG )
        {
            *value = extract_16bits(opt.data);
            DebugFormat(DEBUG_STREAM_STATE, "Found MSS %u\n", *value);
            return TF_MSS;
        }
    }

    *value = 0;

    DebugMessage(DEBUG_STREAM_STATE, "No MSS...\n");

    return TF_NONE;
}

uint32_t TcpSegmentDescriptor::init_wscale(uint16_t* value)
{
    DebugMessage(DEBUG_STREAM_STATE, "Getting wscale...\n");

    TcpOptIterator iter(tcph, pkt);

    for (const TcpOption& opt : iter)
    {
        if (opt.code == TcpOptCode::WSCALE)
        {
            *value = (uint16_t)opt.data[0];
            DebugFormat(DEBUG_STREAM_STATE, "Found wscale %d\n", *value);

            // If scale specified in option is larger than 14, use 14 because of limitation
            // in the math of shifting a 32bit value (max scaled window is 2^30th).
            // See RFC 1323 for details.
            if (*value > 14)
                *value = 14;

            return TF_WSCALE;
        }
    }

    *value = 0;
    DebugMessage(DEBUG_STREAM_STATE, "No wscale...\n");

    return TF_NONE;
}

bool TcpSegmentDescriptor::has_wscale()
{
    uint16_t wscale;

    DebugMessage(DEBUG_STREAM_STATE, "Checking for wscale...\n");

    return ( init_wscale(&wscale) & TF_WSCALE ) != TF_NONE;
}

void TcpSegmentDescriptor::print_tsd()
{
    LogMessage("Tcp Segment Descriptor:\n");
    LogMessage("    seq:    0x%08X\n", seg_seq);
    LogMessage("    ack:    0x%08X\n", seg_ack);
    LogMessage("    win:    %d\n", seg_wnd);
    LogMessage("    end:    0x%08X\n", end_seq);
}

