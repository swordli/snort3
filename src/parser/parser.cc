//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2013-2013 Sourcefire, Inc.
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

#include "parser.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include <pcap.h>
#include <grp.h>
#include <pwd.h>
#include <fnmatch.h>

#include <iostream>
#include <string>

#include "cmd_line.h"
#include "mstring.h"
#include "config_file.h"
#include "keywords.h"
#include "parse_conf.h"
#include "parse_rule.h"
#include "parse_stream.h"
#include "vars.h"

#include "utils/snort_bounds.h"
#include "utils/util.h"
#include "utils/sflsq.h"
#include "utils/sfportobject.h"
#include "main/snort_config.h"
#include "main/shell.h"
#include "main/snort_debug.h"
#include "detection/signature.h"
#include "detection/treenodes.h"
#include "detection/rules.h"
#include "detection/detect.h"
#include "detection/fpcreate.h"
#include "detection/tag.h"
#include "detection/sfrim.h"
#include "protocols/packet.h"
#include "filters/sfthreshold.h"
#include "filters/sfthd.h"
#include "filters/rate_filter.h"
#include "filters/detection_filter.h"
#include "hash/sfghash.h"
#include "sfip/sf_vartable.h"
#include "sfip/sf_ip.h"
#include "sfip/sf_ipvar.h"
#include "time/ppm.h"
#include "packet_io/active.h"
#include "file_api/libs/file_config.h"
#include "actions/actions.h"
#include "managers/event_manager.h"
#include "managers/module_manager.h"

static unsigned parse_errors = 0;
static unsigned parse_warnings = 0;

rule_index_map_t* ruleIndexMap = NULL;   /* rule index -> sid:gid map */

static std::string s_aux_rules;

void parser_append_rules(const char* s)
{
    s_aux_rules += s;
    s_aux_rules += "\n";
}

unsigned get_parse_errors()
{
    unsigned tmp = parse_errors;
    parse_errors = 0;
    return tmp;
}

unsigned get_parse_warnings()
{
    unsigned tmp = parse_warnings;
    parse_warnings = 0;
    return tmp;
}

//-------------------------------------------------------------------------
// private / implementation methods
//-------------------------------------------------------------------------

static void InitParser(void)
{
    parse_rule_init();

    if (ruleIndexMap != NULL)
        RuleIndexMapFree(&ruleIndexMap);

    ruleIndexMap = RuleIndexMapCreate(MAX_RULE_COUNT);

    if (ruleIndexMap == NULL)
    {
        ParseAbort("failed to create rule index map.");
    }
}

static void CreateDefaultRules(SnortConfig* sc)
{
    if (sc == NULL)
        return;

    CreateRuleType(sc, ACTION_PASS, RULE_TYPE__PASS, 0, &sc->Pass);
    CreateRuleType(sc, ACTION_DROP, RULE_TYPE__DROP, 1, &sc->Drop);
    CreateRuleType(sc, ACTION_SDROP, RULE_TYPE__SDROP, 0, &sc->SDrop);
    CreateRuleType(sc, ACTION_ALERT, RULE_TYPE__ALERT, 1, &sc->Alert);
    CreateRuleType(sc, ACTION_LOG, RULE_TYPE__LOG, 1, &sc->Log);
}

static void FreeRuleTreeNodes(SnortConfig* sc)
{
    RuleTreeNode* rtn;
    OptTreeNode* otn;
    PolicyId policyId;
    SFGHASH_NODE* hashNode;

    if (sc->otn_map == NULL)
        return;

    for (hashNode = sfghash_findfirst(sc->otn_map);
        hashNode;
        hashNode = sfghash_findnext(sc->otn_map))
    {
        otn = (OptTreeNode*)hashNode->data;

        /* Autogenerated OTNs along with their respective pseudo RTN
         * will get cleaned up when the OTN is free'd */
        if (otn->generated)
            continue;

        for (policyId = 0;
            policyId < otn->proto_node_num;
            policyId++)
        {
            rtn = getRtnFromOtn(otn, policyId);
            DestroyRuleTreeNode(rtn);

            otn->proto_nodes[policyId] = NULL;
        }
    }
}

static void FreeOutputLists(ListHead* list)
{
    if ( list->AlertList )
        EventManager::release_outputs(list->AlertList);

    if ( list->LogList )
        EventManager::release_outputs(list->LogList);
}

/* Finish processing/setup Port Tables */
static void finish_portlist_table(FastPatternConfig* fp, const char* s, PortTable* pt)
{
    PortTableSortUniqRules(pt);

    if ( fpDetectGetDebugPrintRuleGroupsUnCompiled(fp) )
    {
        LogMessage("***\n***Port-Table : %s Ports/Rules-UnCompiled\n",s);
        PortTablePrintInputEx(pt, rule_index_map_print_index);
    }

    PortTableCompile(pt);

    if ( fpDetectGetDebugPrintRuleGroupsCompiled(fp) )
    {
        LogMessage("***\n***Port-Table : %s Ports/Rules-Compiled\n",s);
        PortTablePrintCompiledEx(pt, rule_index_map_print_index);
        LogMessage("*** End of Compiled Group\n");
    }
}

static RulePortTables* PortTablesNew(void)
{
    RulePortTables* rpt =
        (RulePortTables*)SnortAlloc(sizeof(RulePortTables));

    /* No content rule objects */
    rpt->tcp_nocontent = PortObjectNew();
    if (rpt->tcp_nocontent == NULL)
        ParseAbort("ParseRulesFile nocontent PortObjectNew() failed");
    PortObjectAddPortAny(rpt->tcp_nocontent);

    rpt->udp_nocontent = PortObjectNew();
    if (rpt->udp_nocontent == NULL)
        ParseAbort("ParseRulesFile nocontent PortObjectNew() failed");
    PortObjectAddPortAny(rpt->udp_nocontent);

    rpt->icmp_nocontent = PortObjectNew();
    if (rpt->icmp_nocontent == NULL)
        ParseAbort("ParseRulesFile nocontent PortObjectNew() failed");
    PortObjectAddPortAny(rpt->icmp_nocontent);

    rpt->ip_nocontent = PortObjectNew();
    if (rpt->ip_nocontent == NULL)
        ParseAbort("ParseRulesFile nocontent PortObjectNew() failed");
    PortObjectAddPortAny(rpt->ip_nocontent);

    /* Create the Any-Any Port Objects for each protocol */
    rpt->tcp_anyany = PortObjectNew();
    if (rpt->tcp_anyany == NULL)
        ParseAbort("ParseRulesFile tcp any-any PortObjectNew() failed");
    PortObjectAddPortAny(rpt->tcp_anyany);

    rpt->udp_anyany = PortObjectNew();
    if (rpt->udp_anyany == NULL)
        ParseAbort("ParseRulesFile udp any-any PortObjectNew() failed");
    PortObjectAddPortAny(rpt->udp_anyany);

    rpt->icmp_anyany = PortObjectNew();
    if (rpt->icmp_anyany == NULL)
        ParseAbort("ParseRulesFile icmp any-any PortObjectNew() failed");
    PortObjectAddPortAny(rpt->icmp_anyany);

    rpt->ip_anyany = PortObjectNew();
    if (rpt->ip_anyany == NULL)
        ParseAbort("ParseRulesFile ip PortObjectNew() failed");
    PortObjectAddPortAny(rpt->ip_anyany);

    /* Create the tcp Rules PortTables */
    rpt->tcp_src = PortTableNew();
    if (rpt->tcp_src == NULL)
        ParseAbort("ParseRulesFile tcp-src PortTableNew() failed");

    rpt->tcp_dst = PortTableNew();
    if (rpt->tcp_dst == NULL)
        ParseAbort("ParseRulesFile tcp-dst PortTableNew() failed");

    /* Create the udp Rules PortTables */
    rpt->udp_src = PortTableNew();
    if (rpt->udp_src == NULL)
        ParseAbort("ParseRulesFile udp-src PortTableNew() failed");

    rpt->udp_dst = PortTableNew();
    if (rpt->udp_dst == NULL)
        ParseAbort("ParseRulesFile udp-dst PortTableNew() failed");

    /* Create the icmp Rules PortTables */
    rpt->icmp_src = PortTableNew();
    if (rpt->icmp_src == NULL)
        ParseAbort("ParseRulesFile icmp-src PortTableNew() failed");

    rpt->icmp_dst = PortTableNew();
    if (rpt->icmp_dst == NULL)
        ParseAbort("ParseRulesFile icmp-dst PortTableNew() failed");

    /* Create the ip Rules PortTables */
    rpt->ip_src = PortTableNew();
    if (rpt->ip_src == NULL)
        ParseAbort("ParseRulesFile ip-src PortTableNew() failed");

    rpt->ip_dst = PortTableNew();
    if (rpt->ip_dst == NULL)
        ParseAbort("ParseRulesFile ip-dst PortTableNew() failed");

    /*
     * someday these could be read from snort.conf, something like...
     * 'config portlist: large-rule-count <val>'
     */
    rpt->tcp_src->pt_lrc = DEFAULT_LARGE_RULE_GROUP;
    rpt->tcp_dst->pt_lrc = DEFAULT_LARGE_RULE_GROUP;
    rpt->udp_src->pt_lrc = DEFAULT_LARGE_RULE_GROUP;
    rpt->udp_dst->pt_lrc = DEFAULT_LARGE_RULE_GROUP;
    rpt->icmp_src->pt_lrc= DEFAULT_LARGE_RULE_GROUP;
    rpt->icmp_dst->pt_lrc= DEFAULT_LARGE_RULE_GROUP;
    rpt->ip_src->pt_lrc  = DEFAULT_LARGE_RULE_GROUP;
    rpt->ip_dst->pt_lrc  = DEFAULT_LARGE_RULE_GROUP;

    return rpt;
}

static void PortTablesFinish(RulePortTables* port_tables, FastPatternConfig* fp)
{
    /* TCP-SRC */
    if (fpDetectGetDebugPrintRuleGroupsCompiled(fp))
    {
        LogMessage("*** TCP-Any-Any Port List\n");
        PortObjectPrintEx(port_tables->tcp_anyany,
            rule_index_map_print_index);
    }

    finish_portlist_table(fp, "tcp src", port_tables->tcp_src);
    finish_portlist_table(fp, "tcp dst", port_tables->tcp_dst);

    /* UDP-SRC */
    if (fpDetectGetDebugPrintRuleGroupsCompiled(fp))
    {
        LogMessage("*** UDP-Any-Any Port List\n");
        PortObjectPrintEx(port_tables->udp_anyany,
            rule_index_map_print_index);
    }

    finish_portlist_table(fp, "udp src", port_tables->udp_src);
    finish_portlist_table(fp, "udp dst", port_tables->udp_dst);

    /* ICMP-SRC */
    if (fpDetectGetDebugPrintRuleGroupsCompiled(fp))
    {
        LogMessage("*** ICMP-Any-Any Port List\n");
        PortObjectPrintEx(port_tables->icmp_anyany,
            rule_index_map_print_index);
    }

    finish_portlist_table(fp, "icmp src", port_tables->icmp_src);
    finish_portlist_table(fp, "icmp dst", port_tables->icmp_dst);

    /* IP-SRC */
    if (fpDetectGetDebugPrintRuleGroupsCompiled(fp))
    {
        LogMessage("IP-Any-Any Port List\n");
        PortObjectPrintEx(port_tables->ip_anyany,
            rule_index_map_print_index);
    }

    finish_portlist_table(fp, "ip src", port_tables->ip_src);
    finish_portlist_table(fp, "ip dst", port_tables->ip_dst);

    RuleListSortUniq(port_tables->tcp_anyany->rule_list);
    RuleListSortUniq(port_tables->udp_anyany->rule_list);
    RuleListSortUniq(port_tables->icmp_anyany->rule_list);
    RuleListSortUniq(port_tables->ip_anyany->rule_list);
    RuleListSortUniq(port_tables->tcp_nocontent->rule_list);
    RuleListSortUniq(port_tables->udp_nocontent->rule_list);
    RuleListSortUniq(port_tables->icmp_nocontent->rule_list);
    RuleListSortUniq(port_tables->ip_nocontent->rule_list);
}

static void OtnInit(SnortConfig* sc)
{
    if (sc == NULL)
        return;

    /* Don't initialize this more than once */
    if ( sc->otn_map != NULL )
        return;

    /* Init sid-gid -> otn map */
    sc->otn_map = OtnLookupNew();
    if (sc->otn_map == NULL)
        ParseAbort("ParseRulesFile otn_map sfghash_new failed.");
}

#define IFACE_VARS_MAX 128
typedef struct iface_var
{
    char name[128];
    uint32_t net;
    uint32_t netmask;
} iface_var_t;

/****************************************************************************
 *
 * Function  : DefineIfaceVar()
 * Purpose   : Assign network address and network mask to IFACE_ADDR_VARNAME
 *             variable.
 * Arguments : interface name (string) netaddress and netmask (4 octets each)
 * Returns   : void function
 *
 ****************************************************************************/
static void DefineIfaceVar(SnortConfig* sc, char* iname, uint8_t* network, uint8_t* netmask)
{
    char valbuf[32];
    char varbuf[BUFSIZ];

    if ((network == NULL) || (*network == 0))
        return;

    SnortSnprintf(varbuf, BUFSIZ, "%s_ADDRESS", iname);

    SnortSnprintf(valbuf, 32, "%d.%d.%d.%d/%d.%d.%d.%d",
        network[0] & 0xff, network[1] & 0xff, network[2] & 0xff,
        network[3] & 0xff, netmask[0] & 0xff, netmask[1] & 0xff,
        netmask[2] & 0xff, netmask[3] & 0xff);

    VarDefine(sc, varbuf, valbuf);
}

/****************************************************************************
 *
 * Function  : DefineAllIfaceVars()
 * Purpose   : Find all up interfaces and define iface_ADDRESS vars for them
 * Arguments : none
 * Returns   : void function
 *
 ****************************************************************************/
static void DefineAllIfaceVars(SnortConfig* sc)
{
    // FIXIT-L don't come back here on reload unless we are going to find
    // new ifaces.
    /* Cache retrieved devs so if user is running with dropped privs and
     * does a reload, we can use previous values */
    static int num_vars = 0;
    /* Should be more than enough to cover the number of
     * interfaces on a machine */
    static iface_var_t iface_vars[IFACE_VARS_MAX];

    if (num_vars > 0)
    {
        int i;

        for (i = 0; i < num_vars; i++)
        {
            DefineIfaceVar(sc, iface_vars[i].name,
                (uint8_t*)&iface_vars[i].net,
                (uint8_t*)&iface_vars[i].netmask);
        }
    }
#ifndef VALGRIND_TESTING
    else
    {
        char errbuf[PCAP_ERRBUF_SIZE];
        pcap_if_t* alldevs;
        pcap_if_t* dev;
        bpf_u_int32 net, netmask;

        if (pcap_findalldevs(&alldevs, errbuf) == -1)
            return;

        for (dev = alldevs; dev != NULL; dev = dev->next)
        {
            if (pcap_lookupnet(dev->name, &net, &netmask, errbuf) == 0)
            {
                /* We've hit the maximum variables we can cache */
                if (num_vars >= IFACE_VARS_MAX)
                    break;

                SnortSnprintf(iface_vars[num_vars].name,
                    sizeof(iface_vars[num_vars].name), "%s", dev->name);

                DefineIfaceVar(sc, iface_vars[num_vars].name,
                    (uint8_t*)&net,
                    (uint8_t*)&netmask);

                iface_vars[num_vars].net = net;
                iface_vars[num_vars].netmask = netmask;
                num_vars++;
            }
        }

        pcap_freealldevs(alldevs);
    }
#endif
}

static RuleListNode* addNodeToOrderedList(RuleListNode* ordered_list,
    RuleListNode* node, int evalIndex)
{
    RuleListNode* prev;

    prev = ordered_list;

    /* set the eval order for this rule set */
    node->evalIndex = evalIndex;

    if (!prev)
    {
        ordered_list = node;
    }
    else
    {
        while (prev->next)
            prev = prev->next;
        prev->next = node;
    }

    node->next = NULL;

    return ordered_list;
}

static void printRuleListOrder(RuleListNode* node)
{
    char buf[STD_BUF];
    RuleListNode* first_node = node;

    SnortSnprintf(buf, STD_BUF, "Rule application order: ");

    while ( node != NULL )
    {
        SnortSnprintfAppend(buf, STD_BUF, "%s%s",
            node == first_node ? "" : "->", node->name);

        node = node->next;
    }

    LogMessage("%s\n", buf);
}

static void IntegrityCheckRules(SnortConfig* sc)
{
    OptFpList* ofl_idx = NULL;
    int opt_func_count;
    SFGHASH_NODE* hashNode = NULL;
    OptTreeNode* otn  = NULL;
    PolicyId policyId = 0;
    RuleTreeNode* rtn = NULL;

    for (hashNode = sfghash_findfirst(sc->otn_map);
        hashNode;
        hashNode = sfghash_findnext(sc->otn_map))
    {
        otn = (OptTreeNode*)hashNode->data;

        for (policyId = 0;
            policyId < otn->proto_node_num;
            policyId++)
        {
            rtn = getRtnFromOtn(otn, policyId);

            if (!rtn)
            {
                continue;
            }

            if ((rtn->proto == IPPROTO_TCP) || (rtn->proto == IPPROTO_UDP)
                || (rtn->proto == IPPROTO_ICMP) || (rtn->proto == ETHERNET_TYPE_IP))
            {
                //do operation
                ofl_idx = otn->opt_func;
                opt_func_count = 0;

                while (ofl_idx != NULL)
                {
                    opt_func_count++;
                    //DEBUG_WRAP(DebugMessage(DEBUG_DETECT, "%p->",ofl_idx->OptTestFunc););
                    ofl_idx = ofl_idx->next;
                }

                if (opt_func_count == 0)
                {
                    ParseError("zero Length OTN List");
                }
                //DEBUG_WRAP(DebugMessage(DEBUG_DETECT,"\n"););
            }
        }
    }

    //DEBUG_WRAP(DebugMessage(DEBUG_DETECT, "OK\n"););
}

static void parse_file(SnortConfig* sc, Shell* sh)
{
    const char* fname = sh->get_file();

    if ( !fname || !*fname )
        return;

    push_parse_location(fname);
    sh->configure(sc);
    pop_parse_location();
}

//-------------------------------------------------------------------------
// public methods
//-------------------------------------------------------------------------

/****************************************************************************
 * Function: ParseSnortConf()
 *
 * Read the rules file a line at a time and send each rule to the rule parser
 * This is the first pass of the configuration file.  It parses everything
 * except the rules.
 *
 * Arguments: None
 *
 * Returns:
 *  SnortConfig *
 *      An initialized and configured snort configuration struct.
 *      This struct should be passed on the second pass of the
 *      configuration file to parse the rules.
 *
 ***************************************************************************/
SnortConfig* ParseSnortConf(const SnortConfig* boot_conf)
{
    SnortConfig* sc = new SnortConfig;

    sc->logging_flags = boot_conf->logging_flags;
    sc->warning_flags = boot_conf->warning_flags;
    VarNode* tmp = boot_conf->var_list;

    const char* fname = get_snort_conf();

    if ( !fname )
        fname = "";

    InitParser();

    /* Setup the default rule action anchor points
     * Need to do this now in case we get a user defined rule type */
    CreateDefaultRules(sc);

    sc->port_tables = PortTablesNew();

    OtnInit(sc);

    sc->fast_pattern_config = FastPatternConfigNew();
    sc->event_queue_config = EventQueueConfigNew();
    sc->threshold_config = ThresholdConfigNew();
    sc->rate_filter_config = RateFilter_ConfigNew();
    sc->detection_filter_config = DetectionFilterConfigNew();
    sc->ip_proto_only_lists = (SF_LIST**)SnortAlloc(NUM_IP_PROTOS * sizeof(SF_LIST*));

    /* If snort is not run with root privileges, no interfaces will be defined,
     * so user beware if an iface_ADDRESS variable is used in snort.conf and
     * snort is not run as root (even if just in read mode) */
    DefineAllIfaceVars(sc);

    /* Add command line defined variables - duplicates will already
     * have been resolved */
    while (tmp != NULL)
    {
        AddVarToTable(sc, tmp->name, tmp->value);
        tmp = tmp->next;
    }

    // get overrides from cmd line
    Shell* sh = boot_conf->policy_map->get_shell();
    sc->policy_map->get_shell()->set_overrides(sh);

    if ( *fname )
    {
        Shell* sh = sc->policy_map->get_shell();
        sh->set_file(fname);
    }

    for ( unsigned i = 0; true; i++ )
    {
        Shell* sh = sc->policy_map->get_shell(i);

        if ( !sh )
            break;

        set_policies(sc, i);
        parse_file(sc, sh);
    }
    set_policies(sc);
    return sc;
}

void FreeRuleTreeNode(RuleTreeNode* rtn)
{
    RuleFpList* idx, * tmp;
    if (!rtn)
        return;

    if (rtn->sip)
    {
        sfvar_free(rtn->sip);
    }

    if (rtn->dip)
    {
        sfvar_free(rtn->dip);
    }

    idx = rtn->rule_func;
    while (idx)
    {
        tmp = idx;
        idx = idx->next;
        free(tmp);
    }
}

void DestroyRuleTreeNode(RuleTreeNode* rtn)
{
    if (!rtn)
        return;

    rtn->otnRefCount--;
    if (rtn->otnRefCount != 0)
        return;

    FreeRuleTreeNode(rtn);

    free(rtn);
}

/****************************************************************************
 * Purpose: Adjust the information for a given rule
 *          relative to the Rule State list
 *****************************************************************************/
void SetRuleStates(SnortConfig* sc)
{
    RuleState* rule_state;

    if (sc == NULL)
        return;

    /* First, cycle through the rule state list and update the
     * rule state for each one we find. */
    for (rule_state = sc->rule_state_list; rule_state != NULL; rule_state = rule_state->next)
    {
        /* Lookup the OTN by ruleState->sid, ruleState->gid */
        OptTreeNode* otn = OtnLookup(sc->otn_map, rule_state->gid, rule_state->sid);

        if (otn == NULL)
        {
            ParseError("Rule state specified for invalid SID: %d GID: %d",
                rule_state->sid, rule_state->gid);
            return;
        }

        otn->enabled = rule_state->state;
    }
}

void ParserCleanup(void)
{
    parse_rule_term();

    if (ruleIndexMap != NULL)
    {
        RuleIndexMapFree(&ruleIndexMap);
        ruleIndexMap = NULL;
    }
}

void ParseRules(SnortConfig* sc)
{
    for ( unsigned idx = 0; idx < sc->policy_map->ips_policy.size(); ++idx )
    {
        set_policies(sc, idx);
        IpsPolicy* p = sc->policy_map->ips_policy[idx];

        if ( p->enable_builtin_rules )
            ModuleManager::load_rules(sc);

        const char* fname = p->include.c_str();

        if ( fname && *fname )
        {
            push_parse_location(fname);
            ParseConfigFile(sc, fname);
            pop_parse_location();
        }

        if ( !idx )
            p->rules += s_aux_rules;

        if ( !p->rules.empty() )
        {
            push_parse_location("rules");
            ParseConfigString(sc, p->rules.c_str());
            pop_parse_location();
        }
        if ( !idx && sc->stdin_rules )
        {
            LogMessage("Reading rules until EOF or a line starting with END\n");
            push_parse_location("stdin");
            parse_stream(std::cin, sc);
            pop_parse_location();
        }
    }
    IntegrityCheckRules(sc);
    /*FindMaxSegSize();*/

    /* Compile/Finish and Print the PortList Tables */
    PortTablesFinish(sc->port_tables, sc->fast_pattern_config);

    parse_rule_print();
}

/****************************************************************************
 *
 * Function: CreateRuleType
 *
 * Purpose: Creates a new type of rule and adds it to the end of the rule list
 *
 * Arguments: name = name of this rule type
 *                       mode = the mode for this rule type
 *                   rval = return value for this rule type (for detect events)
 *                       head = list head to use (or NULL to create a new one)
 *
 * Returns: the ListHead for the rule type
 *
 ***************************************************************************/
ListHead* CreateRuleType(SnortConfig* sc, const char* name,
    RuleType mode, int rval, ListHead* head)
{
    RuleListNode* node;
    int evalIndex = 0;

    if (sc == NULL)
        return NULL;

    node = (RuleListNode*)SnortAlloc(sizeof(RuleListNode));

    /* If this is the first rule list node, then we need to
     * create a new list. */
    if (sc->rule_lists == NULL)
    {
        sc->rule_lists = node;
    }
    else
    {
        RuleListNode* tmp = sc->rule_lists;
        RuleListNode* last;

        do
        {
            /* We do not allow multiple rules types with the same name. */
            if (strcasecmp(tmp->name, name) == 0)
            {
                free(node);
                return NULL;
            }

            evalIndex++;
            last = tmp;
            tmp = tmp->next;
        }
        while (tmp != NULL);

        last->next = node;
    }

    /* User defined rule type so we need to create a list head for it */
    if (head == NULL)
    {
        node->RuleList = (ListHead*)SnortAlloc(sizeof(ListHead));
    }
    else
    {
        /* Our default rules already have list heads */
        node->RuleList = head;
    }

    node->RuleList->ruleListNode = node;
    node->mode = mode;
    node->rval = rval;
    node->name = SnortStrdup(name);
    node->evalIndex = evalIndex;

    sc->evalOrder[node->mode] =  evalIndex;
    sc->num_rule_types++;

    return node->RuleList;
}

void FreeRuleLists(SnortConfig* sc)
{
    if (sc == NULL)
        return;

    FreeRuleTreeNodes(sc);

    FreeOutputLists(&sc->Drop);
    FreeOutputLists(&sc->SDrop);
    FreeOutputLists(&sc->Alert);
    FreeOutputLists(&sc->Log);
    FreeOutputLists(&sc->Pass);

    /* Iterate through the user-defined types */
    if (sc->rule_lists != NULL)
    {
        RuleListNode* node = sc->rule_lists;

        while (node != NULL)
        {
            RuleListNode* tmp = node;
            node = node->next;

            if ( (tmp->RuleList != &sc->Drop) &&
                (tmp->RuleList != &sc->SDrop) &&
                (tmp->RuleList != &sc->Alert) &&
                (tmp->RuleList != &sc->Log) &&
                (tmp->RuleList != &sc->Pass) )
            {
                FreeOutputLists(tmp->RuleList);
                free(tmp->RuleList);
            }

            if (tmp->name)
                free(tmp->name);

            free(tmp);
        }

        sc->rule_lists = NULL;
    }
}

void PortTablesFree(RulePortTables* port_tables)
{
    if (port_tables == NULL)
        return;

    if (port_tables->tcp_src)
        PortTableFree(port_tables->tcp_src);
    if (port_tables->tcp_dst)
        PortTableFree(port_tables->tcp_dst);
    if (port_tables->udp_src)
        PortTableFree(port_tables->udp_src);
    if (port_tables->udp_dst)
        PortTableFree(port_tables->udp_dst);
    if (port_tables->icmp_src)
        PortTableFree(port_tables->icmp_src);
    if (port_tables->icmp_dst)
        PortTableFree(port_tables->icmp_dst);
    if (port_tables->ip_src)
        PortTableFree(port_tables->ip_src);
    if (port_tables->ip_dst)
        PortTableFree(port_tables->ip_dst);

    if (port_tables->tcp_anyany)
        PortObjectFree(port_tables->tcp_anyany);
    if (port_tables->udp_anyany)
        PortObjectFree(port_tables->udp_anyany);
    if (port_tables->icmp_anyany)
        PortObjectFree(port_tables->icmp_anyany);
    if (port_tables->ip_anyany)
        PortObjectFree(port_tables->ip_anyany);

    if (port_tables->tcp_nocontent)
        PortObjectFree(port_tables->tcp_nocontent);
    if (port_tables->udp_nocontent)
        PortObjectFree(port_tables->udp_nocontent);
    if (port_tables->icmp_nocontent)
        PortObjectFree(port_tables->icmp_nocontent);
    if (port_tables->ip_nocontent)
        PortObjectFree(port_tables->ip_nocontent);

    free(port_tables);
}

void PrintRuleOrder(RuleListNode* rule_lists)
{
    printRuleListOrder(rule_lists);
}

/****************************************************************************
 *
 * Function: OrderRuleLists
 *
 * Purpose: Orders the rule lists into the specefied order.
 *
 * Returns: void function
 *
 ***************************************************************************/
void OrderRuleLists(SnortConfig* sc, const char* order)
{
    int i;
    int evalIndex = 0;
    RuleListNode* ordered_list = NULL;
    RuleListNode* prev;
    RuleListNode* node;
    char** toks;
    int num_toks;

    toks = mSplit(order, " \t", 0, &num_toks, 0);

    for ( i = 0; i < num_toks; i++ )
    {
        prev = NULL;
        node = sc->rule_lists;

        while (node != NULL)
        {
            if (strcmp(toks[i], node->name) == 0)
            {
                if (prev == NULL)
                    sc->rule_lists = node->next;
                else
                    prev->next = node->next;

                /* Add node to ordered list */
                ordered_list = addNodeToOrderedList(ordered_list, node, evalIndex++);
                sc->evalOrder[node->mode] =  evalIndex;

                break;
            }
            else
            {
                prev = node;
                node = node->next;
            }
        }

        if ( node == NULL )
        {
            ParseError("ruletype '%s' does not exist or "
                "has already been ordered.", toks[i]);
            return;
        }
    }

    mSplitFree(&toks, num_toks);

    /* anything left in the rule lists needs to be moved to the ordered lists */
    while (sc->rule_lists != NULL)
    {
        node = sc->rule_lists;
        sc->rule_lists = node->next;
        /* Add node to ordered list */
        ordered_list = addNodeToOrderedList(ordered_list, node, evalIndex++);
        sc->evalOrder[node->mode] =  evalIndex;
    }

    /* set the rulelists to the ordered list */
    sc->rule_lists = ordered_list;
}

NORETURN void ParseAbort(const char* format, ...)
{
    char buf[STD_BUF+1];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, STD_BUF, format, ap);
    va_end(ap);

    buf[STD_BUF] = '\0';

    const char* file_name;
    unsigned file_line;
    get_parse_location(file_name, file_line);

    if (file_name != NULL)
        FatalError("%s(%d) %s\n", file_name, file_line, buf);
    else
        FatalError("%s\n", buf);
}

void ParseError(const char* format, ...)
{
    char buf[STD_BUF+1];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, STD_BUF, format, ap);
    va_end(ap);

    buf[STD_BUF] = '\0';

    const char* file_name;
    unsigned file_line;
    get_parse_location(file_name, file_line);

    if (file_line )
        LogMessage("ERROR: %s:%d %s\n", file_name, file_line, buf);
    else
        LogMessage("ERROR: %s\n", buf);

    parse_errors++;
}

void ParseWarning(WarningGroup wg, const char* format, ...)
{
    if ( !(snort_conf->warning_flags & (1 << wg)) )
        return;

    char buf[STD_BUF+1];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, STD_BUF, format, ap);
    va_end(ap);

    buf[STD_BUF] = '\0';

    const char* file_name;
    unsigned file_line;
    get_parse_location(file_name, file_line);

    if ( file_line )
        LogMessage("WARNING: %s:%d %s\n", file_name, file_line, buf);
    else
        LogMessage("WARNING: %s\n", buf);

    parse_warnings++;
}

void ParseMessage(const char* format, ...)
{
    char buf[STD_BUF+1];
    va_list ap;

    va_start(ap, format);
    vsnprintf(buf, STD_BUF, format, ap);
    va_end(ap);

    buf[STD_BUF] = '\0';

    const char* file_name;
    unsigned file_line;
    get_parse_location(file_name, file_line);

    if (file_name != NULL)
        LogMessage("%s(%d) %s\n", file_name, file_line, buf);
    else
        LogMessage("%s\n", buf);
}

/**Delete rtn from OTN.
 *
 * @param otn pointer to structure OptTreeNode.
 * @param policyId policy id
 *
 * @return pointer to deleted RTN, NULL otherwise.
 */
RuleTreeNode* deleteRtnFromOtn(OptTreeNode* otn, PolicyId policyId)
{
    RuleTreeNode* rtn = NULL;

    if (otn->proto_nodes
        && (otn->proto_node_num >= (policyId+1)))
    {
        rtn = getRtnFromOtn(otn, policyId);
        otn->proto_nodes[policyId] = NULL;

        return rtn;
    }

    return NULL;
}

RuleTreeNode* deleteRtnFromOtn(OptTreeNode* otn)
{
    return deleteRtnFromOtn(otn, get_ips_policy()->policy_id);
}

/**Add RTN to OTN for a particular OTN.
 * @param otn pointer to structure OptTreeNode.
 * @param policyId policy id
 * @param rtn pointer to RuleTreeNode structure
 *
 * @return 0 if successful,
 *         -ve otherwise
 */
int addRtnToOtn(
    OptTreeNode* otn,
    RuleTreeNode* rtn,
    PolicyId policyId
    )
{
    if (otn->proto_node_num <= policyId)
    {
        /* realloc the list, initialize missing elements to 0 and add
         * policyId */
        unsigned int numNodes = (policyId + 1);
        RuleTreeNode** tmpNodeArray = (RuleTreeNode**)SnortAlloc(sizeof(RuleTreeNode*) * numNodes);

        /* copy original contents, the remaining elements are already
         * zeroed out by snortAlloc */
        if (otn->proto_nodes)
        {
            memcpy(tmpNodeArray, otn->proto_nodes,
                sizeof(RuleTreeNode*) * otn->proto_node_num);
            free(otn->proto_nodes);
        }

        otn->proto_node_num = numNodes;
        otn->proto_nodes = tmpNodeArray;
    }

    //add policyId
    if (otn->proto_nodes[policyId])
    {
        DestroyRuleTreeNode(rtn);
        return -1;
    }

    otn->proto_nodes[policyId] = rtn;

    return 0; //success
}

int addRtnToOtn(OptTreeNode* otn, RuleTreeNode* rtn)
{
    return addRtnToOtn(otn, rtn, get_ips_policy()->policy_id);
}

void rule_index_map_print_index(int index, char* buf, int bufsize)
{
    if ( index < ruleIndexMap->num_rules )
    {
        SnortSnprintfAppend(buf, bufsize, "%u:%u ",
            ruleIndexMap->map[index].gid,
            ruleIndexMap->map[index].sid);
    }
}

