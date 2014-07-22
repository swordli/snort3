/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2002-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
// config_ipv6_frag.cc author Josh Rosenbaum <jrosenba@cisco.com>

#include <sstream>
#include <vector>

#include "conversion_state.h"
#include "utils/converter.h"
#include "utils/snort2lua_util.h"

namespace config
{

namespace {

class Ipv6Frag : public ConversionState
{
public:
    Ipv6Frag(Converter* cv, LuaData* ld) : ConversionState(cv, ld) {};
    virtual ~Ipv6Frag() {};
    virtual bool convert(std::istringstream& data_stream);

private:
    void add_deleted_option(std::string opt);
};

} // namespace

void Ipv6Frag::add_deleted_option(std::string dlt_opt)
{
    // see comment in Ipv6Frag::convert
    if (!ld->is_quiet_mode())
        ld->add_deleted_comment("config ipv6_frag: " + dlt_opt);
}

bool Ipv6Frag::convert(std::istringstream& data_stream)
{
    bool retval = true;
    std::string arg;

    // I'm checking here because I do not want to create this
    // table in quiet mode
    if (!ld->is_quiet_mode())
        ld->open_table("deleted_snort_config_options");

    while (util::get_string(data_stream, arg, ","))
    {
        bool tmpval = true;
        std::string keyword;
        std::istringstream arg_stream(arg);

        if (!(arg_stream >> keyword))
            tmpval = false;

        else if (!keyword.compare("max_frag_sessions"))
            add_deleted_option("max_frag_sessions");

        else if (!keyword.compare("bsd_icmp_frag_alert"))
            add_deleted_option("config ipv6_frag: bsd_icmp_frag_alert");

        else if (!keyword.compare("bad_ipv6_frag_alert"))
            add_deleted_option("bad_ipv6_frag_alert");

        else if (!keyword.compare("drop_bad_ipv6_frag"))
            add_deleted_option("drop_bad_ipv6_frag");

        else if (!keyword.compare("frag_timeout"))
        {
            ld->open_top_level_table("ip_stream");
            tmpval = parse_int_option("session_timeout", arg_stream);
            ld->close_table();
        }

        else
        {
            tmpval = false;
        }

        if (retval && !tmpval)
            retval = false;
    }

    return retval;
}

/**************************
 *******  A P I ***********
 **************************/

static ConversionState* ctor(Converter* cv, LuaData* ld)
{
    return new Ipv6Frag(cv, ld);
}

static const ConvertMap ipv6_frag_api =
{
    "ipv6_frag",
    ctor,
};

const ConvertMap* ipv6_frag_map = &ipv6_frag_api;

} // namespace config
