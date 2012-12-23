/* -*- mode: c; c-file-style: "openbsd" -*- */
/*
 * Copyright (c) 2012 Vincent Bernat <bernat@luffy.cx>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "lldpd.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define NETLINK_BUFFER 4096

struct netlink_req {
    struct nlmsghdr hdr;
    struct rtgenmsg gen;
};

/**
 * Connect to netlink.
 *
 * Open a Netlink socket and connect to it.
 *
 * @param protocol Which protocol to use (eg NETLINK_ROUTE).
 * @return The opened socket or -1 on error.
 */
static int
netlink_connect(int protocol)
{
    int s = 0;
    struct sockaddr_nl local = {
        .nl_family = AF_NETLINK,
        .nl_pid = getpid(),
        .nl_groups = 0
    };

    /* Open Netlink socket */
    log_debug("netlink", "opening netlink socket");
    s = socket(AF_NETLINK, SOCK_RAW, protocol);
    if (s == -1) {
        log_warn("netlink", "unable to open netlink socket");
        return -1;
    }
    if (bind(s, (struct sockaddr *)&local, sizeof(struct sockaddr_nl)) < 0) {
        log_warn("netlink", "unable to bind netlink socket");
        close(s);
        return -1;
    }
    return s;
}

/**
 * Send a netlink message.
 *
 * The type of the message can be chosen as well the route family. The
 * mesage will always be NLM_F_REQUEST | NLM_F_DUMP.
 *
 * @param s      the netlink socket
 * @param type   the request type (eg RTM_GETLINK)
 * @param family the rt family (eg AF_PACKET)
 * @return 0 on success, -1 otherwise
 */
static int
netlink_send(int s, int type, int family)
{
    struct netlink_req req = {
        .hdr = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = 1,
            .nlmsg_pid = getpid() },
        .gen = { .rtgen_family = AF_PACKET }
    };
    struct iovec iov = {
        .iov_base = &req,
        .iov_len = req.hdr.nlmsg_len
    };
    struct sockaddr_nl peer = { .nl_family = AF_NETLINK };
    struct msghdr rtnl_msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_name = &peer,
        .msg_namelen = sizeof(struct sockaddr_nl)
    };

    /* Send netlink message. This is synchronous but we are guaranteed
     * to not block. */
    log_debug("netlink", "sending netlink message");
    if (sendmsg(s, (struct msghdr *)&rtnl_msg, 0) == -1) {
        log_warn("netlink", "unable to send netlink message");
        return -1;
    }

    return 0;
}

/**
 * Free an interface.
 *
 * @param iff interface to be freed
 */
static void
netlink_free_interface(struct netlink_interface *iff)
{
    if (!iff) return;
    free(iff->name);
    free(iff->alias);
    free(iff->address);
    free(iff);
}

/**
 * Free a list of interfaces.
 *
 * @param ifs list of interfaces to be freed
 */
void
netlink_free_interfaces(struct netlink_interface_list *ifs)
{
    struct netlink_interface *iff, *iff_next;
    if (!ifs) return;
    for (iff = TAILQ_FIRST(ifs);
         iff != NULL;
         iff = iff_next) {
        iff_next = TAILQ_NEXT(iff, next);
        netlink_free_interface(iff);
    }
    free(ifs);
}

/**
 * Free a list of addresses.
 *
 * @param ifaddrs list of addresses
 */
void
netlink_free_addresses(struct netlink_address_list *ifaddrs)
{
    struct netlink_address *ifa, *ifa_next;
    if (!ifaddrs) return;
    for (ifa = TAILQ_FIRST(ifaddrs);
         ifa != NULL;
         ifa = ifa_next) {
        ifa_next = TAILQ_NEXT(ifa, next);
        free(ifa);
    }
    free(ifaddrs);
}

/**
 * Parse a `link` netlink message.
 *
 * @param msg  message to be parsed
 * @param iff  where to put the result
 * return 0 if the interface is worth it, -1 otherwise
 */
static int
netlink_parse_link(struct nlmsghdr *msg,
    struct netlink_interface *iff)
{
    struct ifinfomsg *ifi;
    struct rtattr *attribute;
    int len;
    ifi = NLMSG_DATA(msg);
    len = msg->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));

    iff->index = ifi->ifi_index;
    iff->type  = ifi->ifi_type;
    iff->flags = ifi->ifi_flags;
    iff->link  = -1;

    for (attribute = IFLA_RTA(ifi);
         RTA_OK(attribute, len);
         attribute = RTA_NEXT(attribute, len)) {
        switch(attribute->rta_type) {
        case IFLA_IFNAME:
            /* Interface name */
            iff->name = strdup(RTA_DATA(attribute));
            break;
        case IFLA_IFALIAS:
            /* Interface alias */
            iff->alias = strdup(RTA_DATA(attribute));
            break;
        case IFLA_ADDRESS:
            /* Interface MAC address */
            iff->address = malloc(RTA_PAYLOAD(attribute));
            if (iff->address)
                memcpy(iff->address, RTA_DATA(attribute), RTA_PAYLOAD(attribute));
            break;
        case IFLA_LINK:
            /* Index of "inferior" interface */
            iff->link = *(int*)RTA_DATA(attribute);
            break;
        case IFLA_MASTER:
            /* Index of master interface */
            iff->master = *(int*)RTA_DATA(attribute);
            break;
        case IFLA_TXQLEN:
            /* Transmit queue length */
            iff->txqueue = *(int*)RTA_DATA(attribute);
            break;
        case IFLA_MTU:
            /* Maximum Transmission Unit */
            iff->mtu = *(int*)RTA_DATA(attribute);
            break;
        default:
            log_debug("netlink", "unhandled attribute type %d for iface %s",
                      attribute->rta_type, iff->name ? iff->name : "(unknown)");
            break;
        }
    }
    if (!iff->name || !iff->address) {
        log_info("netlink", "interface %d does not have a name or an address, skip",
          iff->index);
        return -1;
    }
    return 0;
}

/**
 * Parse a `address` netlink message.
 *
 * @param msg  message to be parsed
 * @param ifa  where to put the result
 * return 0 if the address is worth it, -1 otherwise
 */
static int
netlink_parse_address(struct nlmsghdr *msg,
    struct netlink_address *ifa)
{
    struct ifaddrmsg *ifi;
    struct rtattr *attribute;
    int len;
    ifi = NLMSG_DATA(msg);
    len = msg->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));

    ifa->index = ifi->ifa_index;
    ifa->flags = ifi->ifa_flags;
    switch (ifi->ifa_family) {
    case AF_INET:
    case AF_INET6: break;
    default:
        log_debug("netlink", "got a non IP address on if %d (family: %d)",
          ifa->index, ifi->ifa_family);
        return -1;
    }

    for (attribute = IFA_RTA(ifi);
         RTA_OK(attribute, len);
         attribute = RTA_NEXT(attribute, len)) {
        switch(attribute->rta_type) {
        case IFA_ADDRESS:
            /* Address */
            if (ifi->ifa_family == AF_INET) {
                struct sockaddr_in ip = { .sin_family = AF_INET };
                memcpy(&ip.sin_addr, RTA_DATA(attribute),
                  sizeof(struct in_addr));
                memcpy(&ifa->address, &ip, sizeof(struct sockaddr_in));
            } else {
                struct sockaddr_in6 ip6 = { .sin6_family = AF_INET6 };
                memcpy(&ip6.sin6_addr, RTA_DATA(attribute),
                  sizeof(struct in6_addr));
                memcpy(&ifa->address, &ip6, sizeof(struct sockaddr_in6));
            }
            break;
        default:
            log_debug("netlink", "unhandled attribute type %d for iface %d",
                      attribute->rta_type, ifa->index);
            break;
        }
    }
    if (ifa->address.ss_family == AF_UNSPEC) {
        log_debug("netlink", "no IP for interface %d",
          ifa->index);
        return -1;
    }
    return 0;
}

/**
 * Receive netlink answer from the kernel.
 *
 * @param s    the netlink socket
 * @param ifs  list to store interface list or NULL if we don't
 * @param ifas list to store address list or NULL if we don't
 * @return     0 on success, -1 on error
 */
static int
netlink_recv(int s,
  struct netlink_interface_list *ifs,
  struct netlink_address_list *ifas)
{
    char reply[NETLINK_BUFFER] __attribute__ ((aligned));
    int  end = 0;

    struct netlink_interface *iff;
    struct netlink_address *ifa;

    while (!end) {
        int len;
        struct nlmsghdr *msg;
        struct iovec iov = {
            .iov_base = reply,
            .iov_len = NETLINK_BUFFER
        };
        struct sockaddr_nl peer = { .nl_family = AF_NETLINK };
        struct msghdr rtnl_reply = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_name = &peer,
            .msg_namelen = sizeof(struct sockaddr_nl)
        };

        len = recvmsg(s, &rtnl_reply, 0);
        if (len == -1) {
            log_warnx("netlink", "unable to receive netlink answer");
            return -1;
        }
        if (!len) return 0;
        for (msg = (struct nlmsghdr*)(void*)reply;
             NLMSG_OK(msg, len);
             msg = NLMSG_NEXT(msg, len)) {
            switch (msg->nlmsg_type) {
            case NLMSG_DONE:
                log_debug("netlink", "received end of dump message");
                end = 1;
                break;
            case RTM_NEWLINK:
                if (!ifs) break;
                log_debug("netlink", "received link information");
                iff = calloc(1, sizeof(struct netlink_interface));
                if (iff == NULL) {
                    log_warn("netlink", "not enough memory for another interface, give what we have");
                    return 0;
                }
                if (netlink_parse_link(msg, iff) == 0)
                    TAILQ_INSERT_TAIL(ifs, iff, next);
                else
                    netlink_free_interface(iff);
                break;
            case RTM_NEWADDR:
                if (!ifas) break;
                log_debug("netlink", "received address information");
                ifa = calloc(1, sizeof(struct netlink_address));
                if (ifa == NULL) {
                    log_warn("netlink", "not enough memory for another address, give what we have");
                    return 0;
                }
                if (netlink_parse_address(msg, ifa) == 0)
                    TAILQ_INSERT_TAIL(ifas, ifa, next);
                else
                    free(ifa);
                break;
            default:
                log_debug("netlink",
                          "received unhandled message type %d (len: %d)",
                          msg->nlmsg_type, msg->nlmsg_len);
            }
        }
    }
    return 0;
}

/**
 * Receive the list of interfaces.
 *
 * @return a list of interfaces.
 */
struct netlink_interface_list*
netlink_get_interfaces()
{
    int s;
    struct netlink_interface_list *ifs;

    if ((s = netlink_connect(NETLINK_ROUTE)) == -1)
        return NULL;
    if (netlink_send(s, RTM_GETLINK, AF_PACKET) == -1) {
        close(s);
        return NULL;
    }

    log_debug("netlink", "get the list of available interfaces");
    ifs = malloc(sizeof(struct netlink_interface_list));
    if (ifs == NULL) {
        log_warn("netlink", "not enough memory for interface list");
        return NULL;
    }
    TAILQ_INIT(ifs);
    netlink_recv(s, ifs, NULL);

    close(s);
    return ifs;
}

/**
 * Receive the list of addresses.
 *
 * @return a list of addresses.
 */
struct netlink_address_list*
netlink_get_addresses()
{
    int s;
    struct netlink_address_list *ifaddrs;

    if ((s = netlink_connect(NETLINK_ROUTE)) == -1)
        return NULL;
    if (netlink_send(s, RTM_GETADDR, AF_UNSPEC) == -1) {
        close(s);
        return NULL;
    }

    log_debug("netlink", "get the list of available addresses");
    ifaddrs = malloc(sizeof(struct netlink_address_list));
    if (ifaddrs == NULL) {
        log_warn("netlink", "not enough memory for address list");
        return NULL;
    }
    TAILQ_INIT(ifaddrs);
    netlink_recv(s, NULL, ifaddrs);

    close(s);
    return ifaddrs;
}
