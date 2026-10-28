// SPDX-License-Identifier: GPL-2.0
/*
 * ovnet: OPPO's virtual netdev for interconnect network sharing ("cellular virtual data").
 *
 * The pad uses the phone's mobile data. The data call ends on ovnetN, and the phone is reached over the Wi-Fi P2P
 * link: OplusVirtualConnect runs
 *	ip link set ovnet0 type ovnet bind p2p0 peerv4 <phone p2p v4> peerv6 <phone p2p v6>
 * ovnet0 holds the tethered address and routes. Its frames go out raw on the bound device to the phone, which
 * does the NAT; there is no encapsulation. Received packets for the tethered address are taken off the bound
 * device by a netdev ingress hook and handed to ovnet0.
 *
 * Reimplemented for 4.19 from the behaviour of OPPO's GPL module oplus_network_vnet.ko (6.12, ColorOS 17
 * OPD2515 vendor_dlkm; no public source): rtnl kind "ovnet", IFLA_INFO_DATA 1 = bind ifindex (0 = unbind),
 * 2 = peer IPv4, 3 = peer IPv6; numvnets (3) devices "ovnet%d" at init; sysctl net/ovnet_dev/{enable,debug}.
 */
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/rtnetlink.h>
#include <linux/sysctl.h>
#include <net/addrconf.h>
#include <net/neighbour.h>
#include <net/rtnetlink.h>

enum {
	IFLA_OVNET_UNSPEC,
	IFLA_OVNET_BIND,	/* u32 ifindex of the lower device, 0 = unbind */
	IFLA_OVNET_PEERV4,	/* 4 bytes, network order */
	IFLA_OVNET_PEERV6,	/* 16 bytes */
	__IFLA_OVNET_MAX
};
#define IFLA_OVNET_MAX (__IFLA_OVNET_MAX - 1)

/* check_rx() verdicts */
enum { OVNET_STEAL, OVNET_KEEP, OVNET_COPY };

struct ovnet_priv {
	u32 bind_ifindex;
	__be32 peerv4;
	struct in6_addr peerv6;
	struct net_device __rcu *lower;
	struct nf_hook_ops ops;
};

static int numvnets = 3;
module_param(numvnets, int, 0444);
MODULE_PARM_DESC(numvnets, "Number of ovnet devices made at init");

static int s_enable = 1;
static int s_debug;
static struct ctl_table_header *s_sysctl;
static struct rtnl_link_ops ovnet_link_ops;

#define ovnet_dbg(fmt, ...) do { if (s_debug) pr_info("OVNET: " fmt, ##__VA_ARGS__); } while (0)

static const __be32 v4_24 = htonl(0xffffff00);

static int check_rx(struct sk_buff *skb, struct ovnet_priv *p, struct net_device *ov)
{
	switch (skb->protocol) {
	case htons(ETH_P_ARP):
	case htons(ETH_P_RARP):
		return OVNET_COPY;
	case htons(ETH_P_IP): {
		const struct iphdr *iph;

		if (!pskb_may_pull(skb, sizeof(*iph)))
			return OVNET_KEEP;
		iph = ip_hdr(skb);
		if (!((iph->saddr ^ p->peerv4) & v4_24))
			return OVNET_KEEP;		/* from the P2P subnet itself */
		if (!iph->saddr || iph->daddr == htonl(INADDR_BROADCAST))
			return OVNET_COPY;		/* DHCP and broadcast: both devices */
		return OVNET_STEAL;
	}
	case htons(ETH_P_IPV6): {
		const struct ipv6hdr *ip6h;

		if (!pskb_may_pull(skb, sizeof(*ip6h)))
			return OVNET_KEEP;
		ip6h = ipv6_hdr(skb);
		if (ipv6_addr_is_multicast(&ip6h->daddr))
			return OVNET_COPY;
		return ipv6_chk_addr(dev_net(ov), &ip6h->daddr, ov, 1) ? OVNET_STEAL : OVNET_KEEP;
	}
	default:
		return OVNET_KEEP;
	}
}

static void ovnet_rx(struct sk_buff *skb, struct net_device *ov)
{
	skb->dev = ov;
	skb->skb_iif = ov->ifindex;
	ov->stats.rx_packets++;
	ov->stats.rx_bytes += skb->len;
	netif_receive_skb_core(skb);
}

/* netdev ingress of the bound device (priority 1) */
static unsigned int ovnet_ingress_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
	struct net_device *ov = priv;
	struct sk_buff *copy;

	if (!s_enable || !(ov->flags & IFF_UP))
		return NF_ACCEPT;

	switch (check_rx(skb, netdev_priv(ov), ov)) {
	case OVNET_STEAL:
		ovnet_dbg("rx %s -> %s\n", skb->dev->name, ov->name);
		ovnet_rx(skb, ov);
		return NF_STOLEN;
	case OVNET_COPY:
		copy = skb_copy(skb, GFP_ATOMIC);
		if (copy)
			ovnet_rx(copy, ov);
		return NF_ACCEPT;
	default:
		return NF_ACCEPT;
	}
}

static netdev_tx_t ovnet_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ovnet_priv *p = netdev_priv(dev);
	struct net_device *lower;
	unsigned int len = skb->len;
	__be32 nh4;
	int tbl;
	void *nh;

	switch (skb->protocol) {
	case htons(ETH_P_IP):
	case htons(ETH_P_IPV6):
	case htons(ETH_P_ARP):
	case htons(ETH_P_RARP):
		break;
	default:
		ovnet_dbg("protocol invalid 0x%x\n", ntohs(skb->protocol));
		goto drop;
	}

	rcu_read_lock();
	lower = rcu_dereference(p->lower);
	if (!lower) {
		rcu_read_unlock();
		goto drop;
	}

	if (skb->protocol == htons(ETH_P_IP)) {
		if (!pskb_network_may_pull(skb, sizeof(struct iphdr))) {
			rcu_read_unlock();
			goto drop;
		}
		nh4 = ip_hdr(skb)->daddr;
		/* off the P2P subnet the phone is the gateway */
		if (nh4 != htonl(INADDR_BROADCAST) && ((nh4 ^ p->peerv4) & v4_24))
			nh4 = p->peerv4;
		tbl = NEIGH_ARP_TABLE;
		nh = &nh4;
	} else {
		tbl = NEIGH_ND_TABLE;
		nh = &p->peerv6;
	}

	skb->dev = lower;
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	/* neigh_*_output() pulls our own L2 header and builds the one of the lower device */
	neigh_xmit(tbl, lower, nh, skb);
	rcu_read_unlock();
	return NETDEV_TX_OK;

drop:
	dev->stats.tx_dropped++;
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int ovnet_open(struct net_device *dev)
{
	netif_tx_start_all_queues(dev);
	return 0;
}

static int ovnet_close(struct net_device *dev)
{
	netif_tx_stop_all_queues(dev);
	return 0;
}

static int ovnet_validate_addr(struct net_device *dev)
{
	return 0;
}

static const struct net_device_ops ovnet_netdev_ops = {
	.ndo_open		= ovnet_open,
	.ndo_stop		= ovnet_close,
	.ndo_start_xmit		= ovnet_xmit,
	.ndo_validate_addr	= ovnet_validate_addr,
};

static void ovnet_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &ovnet_netdev_ops;
	dev->rtnl_link_ops = &ovnet_link_ops;
	dev->flags |= IFF_NOARP | IFF_POINTOPOINT;
	dev->priv_flags &= ~(IFF_XMIT_DST_RELEASE | IFF_XMIT_DST_RELEASE_PERM | IFF_TX_SKB_SHARING);
	dev->features |= NETIF_F_LLTX;
	dev->tx_queue_len = 500;
	dev->needs_free_netdev = true;
	eth_hw_addr_random(dev);
}

/* RTNL held */
static void ovnet_unbind(struct net_device *dev)
{
	struct ovnet_priv *p = netdev_priv(dev);
	struct net_device *lower = rtnl_dereference(p->lower);

	if (!lower)
		return;
	ovnet_dbg("%s unbind %s\n", dev->name, lower->name);
	RCU_INIT_POINTER(p->lower, NULL);
	nf_unregister_net_hook(dev_net(lower), &p->ops);
	synchronize_net();
	dev_put(lower);
}

/* RTNL held */
static int ovnet_bind(struct net_device *dev, u32 ifindex)
{
	struct ovnet_priv *p = netdev_priv(dev);
	struct net_device *lower;
	int err;

	ovnet_unbind(dev);
	p->bind_ifindex = ifindex;
	if (!ifindex)
		return 0;

	lower = __dev_get_by_index(dev_net(dev), ifindex);
	if (!lower || lower == dev || lower->rtnl_link_ops == &ovnet_link_ops) {
		p->bind_ifindex = 0;
		return -ENODEV;
	}
	dev_hold(lower);
	p->ops = (struct nf_hook_ops) {
		.hook		= ovnet_ingress_hook,
		.dev		= lower,
		.priv		= dev,
		.pf		= NFPROTO_NETDEV,
		.hooknum	= NF_NETDEV_INGRESS,
		.priority	= 1,
	};
	err = nf_register_net_hook(dev_net(lower), &p->ops);
	if (err) {
		pr_err("OVNET: nf_register_net_hook failed %d\n", err);
		dev_put(lower);
		p->bind_ifindex = 0;
		return err;
	}
	rcu_assign_pointer(p->lower, lower);
	ovnet_dbg("%s bind %s\n", dev->name, lower->name);
	return 0;
}

static int ovnet_apply(struct net_device *dev, struct nlattr *data[])
{
	struct ovnet_priv *p = netdev_priv(dev);
	int err;

	if (!data)
		return 0;
	if (data[IFLA_OVNET_BIND]) {
		u32 ifindex = nla_get_u32(data[IFLA_OVNET_BIND]);

		if (ifindex != p->bind_ifindex) {
			err = ovnet_bind(dev, ifindex);
			if (err)
				return err;
		}
	}
	if (data[IFLA_OVNET_PEERV4])
		nla_memcpy(&p->peerv4, data[IFLA_OVNET_PEERV4], sizeof(p->peerv4));
	if (data[IFLA_OVNET_PEERV6])
		nla_memcpy(&p->peerv6, data[IFLA_OVNET_PEERV6], sizeof(p->peerv6));
	return 0;
}

static int ovnet_validate(struct nlattr *tb[], struct nlattr *data[], struct netlink_ext_ack *extack)
{
	return 0;
}

static int ovnet_newlink(struct net *src_net, struct net_device *dev, struct nlattr *tb[], struct nlattr *data[],
			 struct netlink_ext_ack *extack)
{
	int err = register_netdevice(dev);

	if (err)
		return err;
	err = ovnet_apply(dev, data);
	if (err)
		unregister_netdevice(dev);
	return err;
}

static int ovnet_changelink(struct net_device *dev, struct nlattr *tb[], struct nlattr *data[],
			    struct netlink_ext_ack *extack)
{
	return ovnet_apply(dev, data);
}

static void ovnet_dellink(struct net_device *dev, struct list_head *head)
{
	ovnet_unbind(dev);
	unregister_netdevice_queue(dev, head);
}

static size_t ovnet_get_size(const struct net_device *dev)
{
	return nla_total_size(4) + nla_total_size(4) + nla_total_size(16);
}

static int ovnet_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct ovnet_priv *p = netdev_priv(dev);

	if (nla_put_u32(skb, IFLA_OVNET_BIND, p->bind_ifindex) ||
	    nla_put(skb, IFLA_OVNET_PEERV4, sizeof(p->peerv4), &p->peerv4) ||
	    nla_put(skb, IFLA_OVNET_PEERV6, sizeof(p->peerv6), &p->peerv6))
		return -EMSGSIZE;
	return 0;
}

static const struct nla_policy ovnet_policy[IFLA_OVNET_MAX + 1] = {
	[IFLA_OVNET_BIND]	= { .type = NLA_U32 },
	[IFLA_OVNET_PEERV4]	= { .len = sizeof(__be32) },
	[IFLA_OVNET_PEERV6]	= { .len = sizeof(struct in6_addr) },
};

static struct rtnl_link_ops ovnet_link_ops __read_mostly = {
	.kind		= "ovnet",
	.priv_size	= sizeof(struct ovnet_priv),
	.setup		= ovnet_setup,
	.maxtype	= IFLA_OVNET_MAX,
	.policy		= ovnet_policy,
	.validate	= ovnet_validate,
	.newlink	= ovnet_newlink,
	.changelink	= ovnet_changelink,
	.dellink	= ovnet_dellink,
	.get_size	= ovnet_get_size,
	.fill_info	= ovnet_fill_info,
};

/* the bound device goes away (p2p0 is removed after each P2P group): drop the binding */
static int ovnet_netdev_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct net_device *lower = netdev_notifier_info_to_dev(ptr);
	struct net_device *dev;

	if (event != NETDEV_UNREGISTER || lower->rtnl_link_ops == &ovnet_link_ops)
		return NOTIFY_DONE;
	for_each_netdev(dev_net(lower), dev) {
		struct ovnet_priv *p;

		if (dev->rtnl_link_ops != &ovnet_link_ops)
			continue;
		p = netdev_priv(dev);
		if (rtnl_dereference(p->lower) == lower) {
			pr_info("OVNET: %s unregistering, unbind %s\n", lower->name, dev->name);
			ovnet_unbind(dev);
			p->bind_ifindex = 0;
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block ovnet_notifier = {
	.notifier_call = ovnet_netdev_event,
};

static int zero;
static int one = 1;

static struct ctl_table ovnet_sysctl_table[] = {
	{
		.procname	= "enable",
		.data		= &s_enable,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{
		.procname	= "debug",
		.data		= &s_debug,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{ }
};

static int __init ovnet_init(void)
{
	int err, i;

	err = rtnl_link_register(&ovnet_link_ops);
	if (err)
		return err;
	err = register_netdevice_notifier(&ovnet_notifier);
	if (err)
		goto err_link;

	rtnl_lock();
	for (i = 0; i < numvnets; i++) {
		struct net_device *dev = alloc_netdev(sizeof(struct ovnet_priv), "ovnet%d", NET_NAME_ENUM,
						      ovnet_setup);

		if (!dev) {
			err = -ENOMEM;
			break;
		}
		err = register_netdevice(dev);
		if (err) {
			free_netdev(dev);
			break;
		}
	}
	rtnl_unlock();
	if (err)
		pr_err("OVNET: made %d of %d devices: %d\n", i, numvnets, err);

	s_sysctl = register_net_sysctl(&init_net, "net/ovnet_dev", ovnet_sysctl_table);
	return 0;

err_link:
	rtnl_link_unregister(&ovnet_link_ops);
	return err;
}

static void __exit ovnet_exit(void)
{
	if (s_sysctl)
		unregister_net_sysctl_table(s_sysctl);
	unregister_netdevice_notifier(&ovnet_notifier);
	rtnl_link_unregister(&ovnet_link_ops);	/* dellink on each ovnet device */
}

module_init(ovnet_init);
module_exit(ovnet_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OPPO interconnect virtual netdev (ovnet) over the Wi-Fi P2P link");
MODULE_ALIAS_RTNL_LINK("ovnet");
