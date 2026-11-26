// SPDX-License-Identifier: GPL-2.0-only
// Netlink family 27 ("TCP info dispatcher") for OriginOS 6
/*
 * OriginOS telephony framework opens socket(AF_NETLINK, SOCK_DGRAM, 27) from com.android.phone:
 *   com.vivo.internal.telephony.o2   ("VivoTcpInfoDispatcher", thread "_V_Network_R")
 *     o2.A()  NetlinkUtils.netlinkSocketForProto(27)
 *     o2.W()  build nlmsghdr, NetlinkUtils.sendMessage()
 *     o2$j.handleMessage()
 *
 * Replies are not fabricated. Everything the client does not recognise as
 * type 5/9/10 is parsed as a 29 x s32 CellularTcpInfo and fed to
 * VivoNetworkPrivDataStallTracker, whose "30 s no rx" check runs a data-switch
 * or modem recovery after three consecutive stagnant 10 s samples. Invented
 * counters can therefore reset the modem.
 *
 * Recovered protocol, native endian throughout:
 *   requests  o2.W(uid, type, payload) nlmsg_seq carries the uid
 *     0  -                global enable (seq 0 on first use)
 *     2  ascii, 1024 B    poll uid tcp info, payload "VivoTcpInfo Test!"
 *     3  ascii, 1024 B    start udp-loss monitor, "0 0 24 24 4 9 100"
 *     4  -                stop udp-loss monitor
 *     6  -                screen state, seq 1 = on, 0 = off
 *     7  -                add uid to blacklist
 *     8  -                remove uid from blacklist
 *   replies   o2$a.run() dispatches on nlmsg_type
 *     5  AvCallNetInfo        10 x s32
 *     9  KeyStreamInfo list   s32 count, then 64 B per entry
 *    10  WeChat upload        72 B
 *    21  CellularTcpInfo      29 x s32 (116 B), legacy 15 x s32 if < 112 B
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <net/net_namespace.h>
#include <net/netlink.h>

#define DRV_NAME	"vivo_nl27"
#define NL27_PROTO	27
#define NL27_TYPES	32
#define NL27_REPLY_TYPE	21

static int reply;
module_param(reply, int, 0644);
MODULE_PARM_DESC(reply, "0 = drain only (default), 1 = reply to type 2, 2 = reply to any request");
static int verbose = 1;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "log every received message to the kernel log");

/* CellularTcpInfo, decoded from o2.I(ByteBuffer)*/
struct nl27_tcpinfo {
	s32 uid;
	s32 tcp_state;
	s32 hs_all;
	s32 hs_fail;
	s32 path_mtu;
	s32 rtt;
	s32 rto;
	s32 retransmits;
	s32 rx_pkts;
	s32 tx_pkts;
	s32 ca_state;
	s32 rx_ss_thresh;
	s32 udp_tx_pkts;
	s32 udp_rx_pkts;
	s32 rx_retrans;
	s32 dns_tx_pkts;
	s32 dns_rx_pkts;
	s32 udp_loss;
	s32 f_hs_all;
	s32 f_hs_fail;
	s32 f_rtt;
	s32 f_retrans;
	s32 f_rx_pkts;
	s32 f_tx_pkts;
	s32 f_udp_tx_pkts;
	s32 f_udp_rx_pkts;
	s32 f_rx_retrans;
	s32 f_dns_tx_pkts;
	s32 f_dns_rx_pkts;
};
static struct sock *nl27_sk;
static atomic64_t nl27_types[NL27_TYPES];
static atomic64_t nl27_other;
static atomic64_t nl27_bad;
static atomic64_t nl27_sent;
static void nl27_log_msg(const struct nlmsghdr *nlh, unsigned int skb_len)
{
	char text[65];
	unsigned int plen, n, i;
	plen = nlh->nlmsg_len - NLMSG_HDRLEN;
	n = min_t(unsigned int, plen, sizeof(text) - 1);
	if (n) {
		const unsigned char *p = (const unsigned char *)nlh + NLMSG_HDRLEN;
		for (i = 0; i < n; i++)
			text[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? p[i] : '.';
		text[n] = '\0';
	} else {
		text[0] = '\0';
	}
	pr_info(DRV_NAME ": rx type=%u flags=0x%04x seq=%d pid=%d len=%u skb=%u"
		" payload=\"%s\"\n",
		nlh->nlmsg_type, nlh->nlmsg_flags, nlh->nlmsg_seq, nlh->nlmsg_pid,
		nlh->nlmsg_len, skb_len, text);
}
static void nl27_send_tcpinfo(struct sk_buff *req, const struct nlmsghdr *nlh)
{
	struct nl27_tcpinfo *ti;
	struct nlmsghdr *onlh;
	struct sk_buff *out;
	u32 portid = NETLINK_CB(req).portid;
	out = nlmsg_new(sizeof(*ti), GFP_KERNEL);
	if (!out)
		return;
	onlh = nlmsg_put(out, 0, nlh->nlmsg_seq, NL27_REPLY_TYPE, sizeof(*ti), 0);
	if (!onlh) {
		kfree_skb(out);
		return;
	}
	ti = nlmsg_data(onlh);
	memset(ti, 0, sizeof(*ti));
	ti->uid = (s32)nlh->nlmsg_seq;
	ti->tcp_state = 1;	/* TCP_ESTABLISHED */
	ti->path_mtu = 1500;
	ti->rx_pkts = 1;	/* keep vivo's "no rx" quiet */
	ti->tx_pkts = 1;
	netlink_unicast(nl27_sk, out, portid, MSG_DONTWAIT);
	atomic64_inc(&nl27_sent);
}
static void nl27_input(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	if (unlikely(skb->len < NLMSG_HDRLEN)) {
		atomic64_inc(&nl27_bad);
		return;
	}
	nlh = nlmsg_hdr(skb);
	if (unlikely(nlh->nlmsg_len < NLMSG_HDRLEN ||
		     nlh->nlmsg_len > skb->len)) {
		atomic64_inc(&nl27_bad);
		return;
	}
	if (nlh->nlmsg_type < NL27_TYPES)
		atomic64_inc(&nl27_types[nlh->nlmsg_type]);
	else
		atomic64_inc(&nl27_other);
	if (verbose)
		nl27_log_msg(nlh, skb->len);

	if (reply == 2 || (reply == 1 && nlh->nlmsg_type == 2))
		nl27_send_tcpinfo(skb, nlh);
}
static struct netlink_kernel_cfg nl27_cfg = {
	.input = nl27_input,
};
static int nl27_proc_show(struct seq_file *m, void *v)
{
	int i;
	seq_printf(m, "proto %d %s\n", NL27_PROTO, nl27_sk ? "registered" : "not registered");
	seq_printf(m, "reply %d\n", reply);
	seq_printf(m, "verbose %d\n", verbose);
	seq_printf(m, "bad %llu\n", (unsigned long long)atomic64_read(&nl27_bad));
	seq_printf(m, "other %llu\n", (unsigned long long)atomic64_read(&nl27_other));
	seq_printf(m, "sent %llu\n", (unsigned long long)atomic64_read(&nl27_sent));
	for (i = 0; i < NL27_TYPES; i++) {
		u64 n = atomic64_read(&nl27_types[i]);
		if (n)
			seq_printf(m, "type[%d] %llu\n", i, (unsigned long long)n);
	}
	return 0;
}
static int nl27_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nl27_proc_show, NULL);
}
static const struct proc_ops nl27_proc_ops = {
	.proc_open	= nl27_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
static int __init nl27_init(void)
{
	nl27_sk = netlink_kernel_create(&init_net, NL27_PROTO, &nl27_cfg);
	if (!nl27_sk) {
		pr_err(DRV_NAME ": cannot register netlink family %d\n",
		       NL27_PROTO);
		return -ENOMEM;
	}
	if (!proc_create(DRV_NAME, 0444, NULL, &nl27_proc_ops))
		pr_warn(DRV_NAME ": /proc/%s not created\n", DRV_NAME);
	pr_info(DRV_NAME ": netlink family %d registered, drain only (reply=%d)\n",
		NL27_PROTO, reply);
	return 0;
}
static void __exit nl27_exit(void)
{
	int i;
	remove_proc_entry(DRV_NAME, NULL);
	if (nl27_sk)
		netlink_kernel_release(nl27_sk);
	nl27_sk = NULL;
	for (i = 0; i < NL27_TYPES; i++)
		if (atomic64_read(&nl27_types[i]))
			pr_info(DRV_NAME ": summary type[%d] %llu\n", i, (unsigned long long)atomic64_read(&nl27_types[i]));
	pr_info(DRV_NAME ": summary bad %llu other %llu sent %llu\n",
		(unsigned long long)atomic64_read(&nl27_bad),
		(unsigned long long)atomic64_read(&nl27_other),
		(unsigned long long)atomic64_read(&nl27_sent));
}
module_init(nl27_init);
module_exit(nl27_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vivo TCP info dispatcher");
MODULE_AUTHOR("~jkoo");
MODULE_ALIAS("net-pf-16-proto-27");
//🐥
