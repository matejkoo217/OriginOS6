// SPDX-License-Identifier: GPL-2.0-only
// Netlink family 24 (vivo Wi-Fi "SLA" dispatcher) for OriginOS 6
/*
 * OriginOS Wi-Fi service (inside system_server) opens socket(AF_NETLINK, SOCK_DGRAM, 24)
 * from com.android.server.wifi.qosmonitor.NetLinkDispatcher:
 *   NetLinkDispatcher.connectKernel()          netlinkSocketForProto(24)
 *   NetLinkDispatcher.startSendMsgToKernel()   nlmsghdr + sendMessage()
 *   NetLinkDispatcher$SlaNetLinkDispatcherHandler.handleMessage()
 *
 * vivo has two dispatchers and picks one at runtime from VivoWifiNetlinkDispatcher.getModuleStatus():
 * a "V2" attribute-based one (families 26/30/24 by product) and the legacy "V1" one used here, which
 * hardcodes 24. V2 probes the kernel with a NL_MODULE_SPEC command and gives
 * up after three timeouts, so on a kernel without vivo's netlink module the
 * V2 path reports NOT_SUPPORT and NetLinkDispatcherWrapper builds the V1
 * dispatcher - which is the one that talks to this family.
 *
 * Replies are not fabricated. The only reply this module sends
 * is an empty 16-byte message (no payload) for the request types listed in
 * ack_mask, and only because the client's own parser rejects it:
 *   TcpInfo.parseKernelReply():  if (remaining() < 36) return null;
 *   RttManager's listener:       ... else loge("this info is useless");
 *                                return true;
 *
 * The message is counted as handled (which releases the client's pending
 * reply counter and stops its 100 ms retry loop) but carries no data, so
 * RttInfo keeps its "invalid" value (-1). Without it the client's receive
 * thread never releases its request counter and polls recv() every 100 ms.
 *
 * Recovered protocol (V1 dispatcher), native endian, nlmsg_len 16 or 1024:
 *   request  nlmsghdr[16] + optional ASCII in the 1024 B buffer at offset 16
 *     type = command id, seq = uid (or arg), pid = Process.myPid()
 *     0   start SLA (sent once from the constructor)
 *     9   query video "choke" state, needReply, reply = 4 x s32
 *     10  choke start/stop, ascii "|chokestart" / "|chokestop"
 *     11  get per-uid TCP info, ascii iface ("wlan0", "wlan1",
 *         "mobileiface"), needReply, reply = TcpInfo, >= 36 B:
 *         uid, ifaceType, txPkt, rxPkt, resentPkt, rtt, rttPkt, inDupack,
 *         outDupack [, +5 s32 udp counters if >= 60 B] [, more if >= 76 B]
 *     13  update video thresholds, ascii "<app_id> <hold>"
 *     14  enable/disable video detect, ascii "all"
 *     15  update video params, ascii "<app_id> <hold>"
 *     19  ACC per-uid hint, ascii "2054"
 *     20  ACC per-uid hint
 *     27  start UDP traffic monitor, ascii config
 *     28  start UDP loss monitor, ascii iface
 *     29  stop UDP loss monitor, ascii iface
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

#define DRV_NAME	"vivo_nl24"
#define NL24_PROTO	24
#define NL24_TYPES	32

static int ack_mask = BIT(11);
module_param(ack_mask, int, 0644);
MODULE_PARM_DESC(ack_mask, "bitmask of request types answered with an empty ack (default 1<<11)");
static int verbose;
module_param(verbose, int, 0644);
MODULE_PARM_DESC(verbose, "log every received message to the kernel log");
static struct sock *nl24_sk;
static atomic64_t nl24_types[NL24_TYPES];
static atomic64_t nl24_other;
static atomic64_t nl24_bad;
static atomic64_t nl24_acked;
static void nl24_log_msg(const struct nlmsghdr *nlh, unsigned int skb_len)
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
static void nl24_ack(struct sk_buff *req, const struct nlmsghdr *nlh)
{
	struct nlmsghdr *onlh;
	struct sk_buff *out;
	u32 portid = NETLINK_CB(req).portid;
	out = nlmsg_new(0, GFP_KERNEL);
	if (!out)
		return;
	onlh = nlmsg_put(out, 0, nlh->nlmsg_seq, nlh->nlmsg_type, 0, 0);
	if (!onlh) {
		kfree_skb(out);
		return;
	}
	if (netlink_unicast(nl24_sk, out, portid, MSG_DONTWAIT) >= 0)
		atomic64_inc(&nl24_acked);
}
static void nl24_input(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	if (unlikely(skb->len < NLMSG_HDRLEN)) {
		atomic64_inc(&nl24_bad);
		return;
	}
	nlh = nlmsg_hdr(skb);
	if (unlikely(nlh->nlmsg_len < NLMSG_HDRLEN || nlh->nlmsg_len > skb->len)) {
		atomic64_inc(&nl24_bad);
		return;
	}
	if (nlh->nlmsg_type < NL24_TYPES)
		atomic64_inc(&nl24_types[nlh->nlmsg_type]);
	else
		atomic64_inc(&nl24_other);
	if (verbose)
		nl24_log_msg(nlh, skb->len);
	if (nlh->nlmsg_type < 32 && (ack_mask & BIT(nlh->nlmsg_type)))
		nl24_ack(skb, nlh);
}
static struct netlink_kernel_cfg nl24_cfg = {
	.input = nl24_input,
};
static int nl24_proc_show(struct seq_file *m, void *v)
{
	int i;
	seq_printf(m, "proto %d %s\n", NL24_PROTO, nl24_sk ? "registered" : "not registered");
	seq_printf(m, "ack_mask %d\n", ack_mask);
	seq_printf(m, "verbose %d\n", verbose);
	seq_printf(m, "bad %llu\n", (unsigned long long)atomic64_read(&nl24_bad));
	seq_printf(m, "other %llu\n", (unsigned long long)atomic64_read(&nl24_other));
	seq_printf(m, "acked %llu\n", (unsigned long long)atomic64_read(&nl24_acked));
	for (i = 0; i < NL24_TYPES; i++) {
		u64 n = atomic64_read(&nl24_types[i]);
		if (n)
			seq_printf(m, "type[%d] %llu\n", i, (unsigned long long)n);
	}
	return 0;
}
static int nl24_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, nl24_proc_show, NULL);
}
static const struct proc_ops nl24_proc_ops = {
	.proc_open	= nl24_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
static int __init nl24_init(void)
{
	nl24_sk = netlink_kernel_create(&init_net, NL24_PROTO, &nl24_cfg);
	if (!nl24_sk) {
		pr_err(DRV_NAME ": cannot register netlink family %d\n",
		       NL24_PROTO);
		return -ENOMEM;
	}
	if (!proc_create(DRV_NAME, 0444, NULL, &nl24_proc_ops))
		pr_warn(DRV_NAME ": /proc/%s not created\n", DRV_NAME);
	pr_info(DRV_NAME ": netlink family %d registered, ack_mask=0x%x\n",
		NL24_PROTO, ack_mask);
	return 0;
}
static void __exit nl24_exit(void)
{
	int i;
	remove_proc_entry(DRV_NAME, NULL);
	if (nl24_sk)
		netlink_kernel_release(nl24_sk);
	nl24_sk = NULL;
	for (i = 0; i < NL24_TYPES; i++)
		if (atomic64_read(&nl24_types[i]))
			pr_info(DRV_NAME ": summary type[%d] %llu\n", i, (unsigned long long)atomic64_read(&nl24_types[i]));
	pr_info(DRV_NAME ": summary bad %llu other %llu acked %llu\n",
		(unsigned long long)atomic64_read(&nl24_bad),
		(unsigned long long)atomic64_read(&nl24_other),
		(unsigned long long)atomic64_read(&nl24_acked));
}
// 🐥
module_init(nl24_init);
module_exit(nl24_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Netlink family 24 (vivo Wi-Fi SLA dispatcher) for OriginOS");
MODULE_AUTHOR("~jkoo");
MODULE_ALIAS("net-pf-16-proto-24");
