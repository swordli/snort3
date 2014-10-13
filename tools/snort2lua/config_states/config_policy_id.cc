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
// config_policy_id.cc author Josh Rosenbaum <jrosenba@cisco.com>

#include <sstream>

#include "conversion_state.h"
#include "data/dt_data.h"

namespace config
{

namespace {

class PolicyId : public ConversionState
{
public:
    PolicyId() : ConversionState() {};
    virtual ~PolicyId() {};
    virtual bool convert(std::istringstream& data_stream);
};

} // namespace

bool PolicyId::convert(std::istringstream& data_stream)
{
    bool rc = true;
    int policy_id;

    table_api.open_table("alerts");

    if (data_stream >> policy_id)
    {
        table_api.open_table("ips");
        table_api.add_option("id", policy_id);
        table_api.close_table();

        table_api.open_table("network");
        table_api.add_option("id", policy_id);
        table_api.close_table();
    }
    else
    {
        data_api.failed_conversion(data_stream, "<int>");
        rc = false;
    }

    if (data_stream >> policy_id)
    {
        data_api.failed_conversion(data_stream, std::to_string(policy_id));
        rc = false;
    }

    return rc;
}

/**************************
 *******  A P I ***********
 **************************/

static ConversionState* ctor()
{
    return new PolicyId();
}

static const ConvertMap policy_id_api =
{
    "policy_id",
    ctor,
};

const ConvertMap* policy_id_map = &policy_id_api;

} // namespace config