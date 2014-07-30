/*
 *	MPTCP implementation - IPv6-specific functions
 *
 *	Initial Design & Implementation:
 *	Sébastien Barré <sebastien.barre@uclouvain.be>
 *
 *	Current Maintainer:
 *	Jaakko Korkeaniemi <jaakko.korkeaniemi@aalto.fi>
 *
 *	Additional authors:
 *	Jaakko Korkeaniemi <jaakko.korkeaniemi@aalto.fi>
 *	Gregory Detal <gregory.detal@uclouvain.be>
 *	Fabien Duchêne <fabien.duchene@uclouvain.be>
 *	Andreas Seelinger <Andreas.Seelinger@rwth-aachen.de>
 *	Lavkesh Lahngir <lavkesh51@gmail.com>
 *	Andreas Ripke <ripke@neclab.eu>
 *	Vlad Dogaru <vlad.dogaru@intel.com>
 *	Octavian Purdila <octavian.purdila@intel.com>
 *	John Ronan <jronan@tssg.org>
 *	Catalin Nicutar <catalin.nicutar@gmail.com>
 *	Brandon Heller <brandonh@stanford.edu>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/export.h>
#include <linux/in6.h>
#include <linux/kernel.h>

#include <net/addrconf.h>
#include <net/flow.h>
#include <net/inet6_connection_sock.h>
#include <net/inet6_hashtables.h>
#include <net/inet_common.h>
#include <net/ipv6.h>
#include <net/ip6_checksum.h>
#include <net/ip6_route.h>
#include <net/mptcp.h>
#include <net/mptcp_v6.h>
#include <net/tcp.h>
#include <net/transp_v6.h>

static int mptcp_v6v4_send_synack(struct sock *meta_sk, struct request_sock *req,
				  u16 queue_mapping);

__u32 mptcp_v6_get_nonce(const __be32 *saddr, const __be32 *daddr,
			 __be16 sport, __be16 dport, u32 seq)
{
	u32 secret[MD5_MESSAGE_BYTES / 4];
	u32 hash[MD5_DIGEST_WORDS];
	u32 i;

	memcpy(hash, saddr, 16);
	for (i = 0; i < 4; i++)
		secret[i] = mptcp_secret[i] + (__force u32)daddr[i];
	secret[4] = mptcp_secret[4] +
		    (((__force u16)sport << 16) + (__force u16)dport);
	secret[5] = seq;
	for (i = 6; i < MD5_MESSAGE_BYTES / 4; i++)
		secret[i] = mptcp_secret[i];

	md5_transform(hash, secret);

	return hash[0];
}

u64 mptcp_v6_get_key(const __be32 *saddr, const __be32 *daddr,
		     __be16 sport, __be16 dport)
{
	u32 secret[MD5_MESSAGE_BYTES / 4];
	u32 hash[MD5_DIGEST_WORDS];
	u32 i;

	memcpy(hash, saddr, 16);
	for (i = 0; i < 4; i++)
		secret[i] = mptcp_secret[i] + (__force u32)daddr[i];
	secret[4] = mptcp_secret[4] +
		    (((__force u16)sport << 16) + (__force u16)dport);
	secret[5] = mptcp_key_seed++;
	for (i = 5; i < MD5_MESSAGE_BYTES / 4; i++)
		secret[i] = mptcp_secret[i];

	md5_transform(hash, secret);

	return *((u64 *)hash);
}

static void mptcp_v6_reqsk_destructor(struct request_sock *req)
{
	mptcp_reqsk_destructor(req);

	tcp_v6_reqsk_destructor(req);
}

/* Similar to tcp_v6_rtx_synack */
static int mptcp_v6_rtx_synack(struct sock *meta_sk, struct request_sock *req)
{
	if (meta_sk->sk_family == AF_INET6)
		return tcp_v6_rtx_synack(meta_sk, req);

	TCP_INC_STATS_BH(sock_net(meta_sk), TCP_MIB_RETRANSSEGS);
	return mptcp_v6v4_send_synack(meta_sk, req, 0);
}

/* Similar to tcp6_request_sock_ops */
struct request_sock_ops mptcp6_request_sock_ops __read_mostly = {
	.family		=	AF_INET6,
	.obj_size	=	sizeof(struct mptcp6_request_sock),
	.rtx_syn_ack	=	mptcp_v6_rtx_synack,
	.send_ack	=	tcp_v6_reqsk_send_ack,
	.destructor	=	mptcp_v6_reqsk_destructor,
	.send_reset	=	tcp_v6_send_reset,
	.syn_ack_timeout =	tcp_syn_ack_timeout,
};

static void mptcp_v6_reqsk_queue_hash_add(struct sock *meta_sk,
					  struct request_sock *req,
					  unsigned long timeout)
{
	const u32 h = inet6_synq_hash(&inet6_rsk(req)->rmt_addr,
				      inet_rsk(req)->rmt_port,
				      0, MPTCP_HASH_SIZE);

	inet6_csk_reqsk_queue_hash_add(meta_sk, req, timeout);

	spin_lock(&mptcp_reqsk_hlock);
	list_add(&mptcp_rsk(req)->collide_tuple, &mptcp_reqsk_htb[h]);
	spin_unlock(&mptcp_reqsk_hlock);
}

/* Similar to tcp_v6_send_synack
 *
 * The meta-socket is IPv4, but a new subsocket is IPv6
 */
static int mptcp_v6v4_send_synack(struct sock *meta_sk, struct request_sock *req,
				  u16 queue_mapping)
{
	struct inet6_request_sock *treq = inet6_rsk(req);
	struct sk_buff *skb;
	struct flowi6 fl6;
	struct dst_entry *dst;
	int err = -ENOMEM;

	memset(&fl6, 0, sizeof(fl6));
	fl6.flowi6_proto = IPPROTO_TCP;
	fl6.daddr = treq->rmt_addr;
	fl6.saddr = treq->loc_addr;
	fl6.flowlabel = 0;
	fl6.flowi6_oif = treq->iif;
	fl6.flowi6_mark = meta_sk->sk_mark;
	fl6.fl6_dport = inet_rsk(req)->rmt_port;
	fl6.fl6_sport = inet_rsk(req)->loc_port;
	security_req_classify_flow(req, flowi6_to_flowi(&fl6));

	dst = ip6_dst_lookup_flow(meta_sk, &fl6, NULL, false);
	if (IS_ERR(dst)) {
		err = PTR_ERR(dst);
		return err;
	}
	skb = tcp_make_synack(meta_sk, dst, req, NULL);

	if (skb) {
		__tcp_v6_send_check(skb, &treq->loc_addr, &treq->rmt_addr);

		fl6.daddr = treq->rmt_addr;
		skb_set_queue_mapping(skb, queue_mapping);
		err = ip6_xmit(meta_sk, skb, &fl6, NULL, 0);
		err = net_xmit_eval(err);
	}

	return err;
}

/* Similar to tcp_v6_syn_recv_sock
 *
 * The meta-socket is IPv4, but a new subsocket is IPv6
 */
struct sock *mptcp_v6v4_syn_recv_sock(struct sock *meta_sk, struct sk_buff *skb,
				      struct request_sock *req,
				      struct dst_entry *dst)
{
	struct inet6_request_sock *treq;
	struct ipv6_pinfo *newnp;
	struct tcp6_sock *newtcp6sk;
	struct inet_sock *newinet;
	struct tcp_sock *newtp;
	struct sock *newsk;

	treq = inet6_rsk(req);

	if (sk_acceptq_is_full(meta_sk))
		goto out_overflow;

	if (!dst) {
		/* This code is similar to inet6_csk_route_req, but as we
		 * don't have a np-pointer in the meta, we have to do it
		 * manually.
		 */
		struct flowi6 fl6;

		memset(&fl6, 0, sizeof(fl6));
		fl6.flowi6_proto = IPPROTO_TCP;
		fl6.daddr = treq->rmt_addr;
		fl6.saddr = treq->loc_addr;
		fl6.flowi6_oif = treq->iif;
		fl6.flowi6_mark = meta_sk->sk_mark;
		fl6.fl6_dport = inet_rsk(req)->rmt_port;
		fl6.fl6_sport = inet_rsk(req)->loc_port;
		security_req_classify_flow(req, flowi6_to_flowi(&fl6));

		dst = ip6_dst_lookup_flow(meta_sk, &fl6, NULL, false);
		if (IS_ERR(dst))
			goto out;
	}

	newsk = tcp_create_openreq_child(meta_sk, req, skb);
	if (newsk == NULL)
		goto out_nonewsk;

	/* Diff to tcp_v6_syn_recv_sock: Must do this prior to __ip6_dst_store,
	 * as it tries to access the pinet6-pointer.
	 */
	newtcp6sk = (struct tcp6_sock *)newsk;
	inet_sk(newsk)->pinet6 = &newtcp6sk->inet6;

	/*
	 * No need to charge this sock to the relevant IPv6 refcnt debug socks
	 * count here, tcp_create_openreq_child now does this for us, see the
	 * comment in that function for the gory details. -acme
	 */

	newsk->sk_gso_type = SKB_GSO_TCPV6;
	__ip6_dst_store(newsk, dst, NULL, NULL);
	inet6_sk_rx_dst_set(newsk, skb);

	newtp = tcp_sk(newsk);
	newinet = inet_sk(newsk);
	newnp = inet6_sk(newsk);

	newnp->daddr = treq->rmt_addr;
	newnp->saddr = treq->loc_addr;
	newnp->rcv_saddr = treq->loc_addr;
	newsk->sk_bound_dev_if = treq->iif;

	/* Now IPv6 options...

	   First: no IPv4 options.
	 */
	newinet->inet_opt = NULL;
	newnp->ipv6_ac_list = NULL;
	newnp->ipv6_fl_list = NULL;
	newnp->rxopt.all = 0;

	/* Clone pktoptions received with SYN */
	newnp->pktoptions = NULL;
	if (treq->pktopts != NULL) {
		newnp->pktoptions = skb_clone(treq->pktopts,
					      sk_gfp_atomic(meta_sk, GFP_ATOMIC));
		consume_skb(treq->pktopts);
		treq->pktopts = NULL;
		if (newnp->pktoptions)
			skb_set_owner_r(newnp->pktoptions, newsk);
	}
	newnp->opt	  = NULL;
	newnp->mcast_oif  = inet6_iif(skb);
	newnp->mcast_hops = ipv6_hdr(skb)->hop_limit;
	newnp->rcv_tclass = ipv6_get_dsfield(ipv6_hdr(skb));

	/* Initialization copied from inet6_create - normally this should have
	 * been handled by the memcpy as in tcp_v6_syn_recv_sock
	 */
	newnp->hop_limit  = -1;
	newnp->mc_loop	  = 1;
	newnp->pmtudisc	  = IPV6_PMTUDISC_WANT;
	(void)xchg(&newnp->rxpmtu, NULL);

	inet_csk(newsk)->icsk_ext_hdr_len = 0;

	tcp_mtup_init(newsk);
	tcp_sync_mss(newsk, dst_mtu(dst));
	newtp->advmss = dst_metric_advmss(dst);
	if (tcp_sk(meta_sk)->rx_opt.user_mss &&
	    tcp_sk(meta_sk)->rx_opt.user_mss < newtp->advmss)
		newtp->advmss = tcp_sk(meta_sk)->rx_opt.user_mss;

	tcp_initialize_rcv_mss(newsk);

	newinet->inet_daddr = LOOPBACK4_IPV6;
	newinet->inet_saddr = LOOPBACK4_IPV6;
	newinet->inet_rcv_saddr = LOOPBACK4_IPV6;

	if (__inet_inherit_port(meta_sk, newsk) < 0) {
		inet_csk_prepare_forced_close(newsk);
		tcp_done(newsk);
		goto out;
	}
	__inet6_hash(newsk, NULL);

	return newsk;

out_overflow:
	NET_INC_STATS_BH(sock_net(meta_sk), LINUX_MIB_LISTENOVERFLOWS);
out_nonewsk:
	dst_release(dst);
out:
	NET_INC_STATS_BH(sock_net(meta_sk), LINUX_MIB_LISTENDROPS);
	return NULL;
}

/* Similar to tcp_v6_conn_request */
static void mptcp_v6_join_request(struct sock *meta_sk, struct sk_buff *skb)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct tcp_options_received tmp_opt;
	struct mptcp_options_received mopt;
	struct ipv6_pinfo *np = inet6_sk(meta_sk);
	struct request_sock *req;
	struct inet6_request_sock *treq;
	struct mptcp_request_sock *mtreq;
	u8 mptcp_hash_mac[20];
	__u32 isn = TCP_SKB_CB(skb)->when;
	struct dst_entry *dst = NULL;
	struct flowi6 fl6;
	int want_cookie = 0;
	union inet_addr addr;

	tcp_clear_options(&tmp_opt);
	mptcp_init_mp_opt(&mopt);
	tmp_opt.mss_clamp = TCP_MSS_DEFAULT;
	tmp_opt.user_mss  = tcp_sk(meta_sk)->rx_opt.user_mss;
	tcp_parse_options(skb, &tmp_opt, &mopt, 0, NULL);

	req = inet6_reqsk_alloc(&mptcp6_request_sock_ops);
	if (!req)
		return;

	mtreq = mptcp_rsk(req);
	mtreq->mpcb = mpcb;
	INIT_LIST_HEAD(&mtreq->collide_tuple);

#ifdef CONFIG_TCP_MD5SIG
	tcp_rsk(req)->af_specific = &tcp_request_sock_ipv6_ops;
#endif

	tmp_opt.tstamp_ok = tmp_opt.saw_tstamp;
	tcp_openreq_init(req, &tmp_opt, skb);

	treq = inet6_rsk(req);
	treq->rmt_addr = ipv6_hdr(skb)->saddr;
	treq->loc_addr = ipv6_hdr(skb)->daddr;

	if (!want_cookie || tmp_opt.tstamp_ok)
		TCP_ECN_create_request(req, skb, sock_net(meta_sk));

	treq->iif = meta_sk->sk_bound_dev_if;

	/* So that link locals have meaning */
	if (!meta_sk->sk_bound_dev_if &&
	    ipv6_addr_type(&treq->rmt_addr) & IPV6_ADDR_LINKLOCAL)
		treq->iif = inet6_iif(skb);

	if (!isn) {
		if (meta_sk->sk_family == AF_INET6 &&
		    (ipv6_opt_accepted(meta_sk, skb) ||
		    np->rxopt.bits.rxinfo || np->rxopt.bits.rxoinfo ||
		    np->rxopt.bits.rxhlim || np->rxopt.bits.rxohlim)) {
			atomic_inc(&skb->users);
			treq->pktopts = skb;
		}

		/* VJ's idea. We save last timestamp seen
		 * from the destination in peer table, when entering
		 * state TIME-WAIT, and check against it before
		 * accepting new connection request.
		 *
		 * If "isn" is not zero, this request hit alive
		 * timewait bucket, so that all the necessary checks
		 * are made in the function processing timewait state.
		 */
		if (tmp_opt.saw_tstamp &&
		    tcp_death_row.sysctl_tw_recycle &&
		    (dst = inet6_csk_route_req(meta_sk, &fl6, req)) != NULL) {
			if (!tcp_peer_is_proven(req, dst, true)) {
				NET_INC_STATS_BH(sock_net(meta_sk), LINUX_MIB_PAWSPASSIVEREJECTED);
				goto drop_and_release;
			}
		}
		/* Kill the following clause, if you dislike this way. */
		else if (!sysctl_tcp_syncookies &&
			 (sysctl_max_syn_backlog - inet_csk_reqsk_queue_len(meta_sk) <
			  (sysctl_max_syn_backlog >> 2)) &&
			 !tcp_peer_is_proven(req, dst, false)) {
			/* Without syncookies last quarter of
			 * backlog is filled with destinations,
			 * proven to be alive.
			 * It means that we continue to communicate
			 * to destinations, already remembered
			 * to the moment of synflood.
			 */
			LIMIT_NETDEBUG(KERN_DEBUG "TCP: drop open request from %pI6/%u\n",
				       &treq->rmt_addr, ntohs(tcp_hdr(skb)->source));
			goto drop_and_release;
		}

		isn = tcp_v6_init_sequence(skb);
	}

	tcp_rsk(req)->snt_isn = isn;
	tcp_rsk(req)->snt_synack = tcp_time_stamp;
	tcp_rsk(req)->listener = NULL;

	mtreq->mptcp_rem_nonce = mopt.mptcp_recv_nonce;
	mtreq->mptcp_rem_key = mpcb->mptcp_rem_key;
	mtreq->mptcp_loc_key = mpcb->mptcp_loc_key;
	mtreq->mptcp_loc_nonce = mptcp_v6_get_nonce(ipv6_hdr(skb)->daddr.s6_addr32,
						    ipv6_hdr(skb)->saddr.s6_addr32,
						    tcp_hdr(skb)->dest,
						    tcp_hdr(skb)->source, isn);
	mptcp_hmac_sha1((u8 *)&mtreq->mptcp_loc_key,
			(u8 *)&mtreq->mptcp_rem_key,
			(u8 *)&mtreq->mptcp_loc_nonce,
			(u8 *)&mtreq->mptcp_rem_nonce, (u32 *)mptcp_hash_mac);
	mtreq->mptcp_hash_tmac = *(u64 *)mptcp_hash_mac;

	addr.in6 = treq->loc_addr;
	mtreq->loc_id = mpcb->pm_ops->get_local_id(AF_INET6, &addr, sock_net(meta_sk));
	if (mtreq->loc_id == -1) /* Address not part of the allowed ones */
		goto drop_and_release;
	mtreq->rem_id = mopt.rem_id;
	mtreq->low_prio = mopt.low_prio;
	tcp_rsk(req)->saw_mpc = 1;

	if (meta_sk->sk_family == AF_INET6) {
		if (tcp_v6_send_synack(meta_sk, dst, &fl6, req,
				       skb_get_queue_mapping(skb)))
			goto drop_and_free;
	} else {
		if (mptcp_v6v4_send_synack(meta_sk, req, skb_get_queue_mapping(skb)))
			goto drop_and_free;
	}

	/* Adding to request queue in metasocket */
	mptcp_v6_reqsk_queue_hash_add(meta_sk, req, TCP_TIMEOUT_INIT);

	return;

drop_and_release:
	dst_release(dst);
drop_and_free:
	reqsk_free(req);
	return;
}

int mptcp_v6_rem_raddress(struct mptcp_cb *mpcb, u8 id)
{
	int i;

	for (i = 0; i < MPTCP_MAX_ADDR; i++) {
		if (!((1 << i) & mpcb->rem6_bits))
			continue;

		if (mpcb->remaddr6[i].id == id) {
			/* remove address from bitfield */
			mpcb->rem6_bits &= ~(1 << i);

			return 0;
		}
	}

	return -1;
}

/* Returns -1 if there is no space anymore to store an additional
 * address
 */
int mptcp_v6_add_raddress(struct mptcp_cb *mpcb, const struct in6_addr *addr,
			  __be16 port, u8 id)
{
	int i;
	struct mptcp_rem6 *rem6;

	mptcp_for_each_bit_set(mpcb->rem6_bits, i) {
		rem6 = &mpcb->remaddr6[i];

		/* Address is already in the list --- continue */
		if (rem6->id == id &&
		    ipv6_addr_equal(&rem6->addr, addr) && rem6->port == port)
			return 0;

		/* This may be the case, when the peer is behind a NAT. He is
		 * trying to JOIN, thus sending the JOIN with a certain ID.
		 * However the src_addr of the IP-packet has been changed. We
		 * update the addr in the list, because this is the address as
		 * OUR BOX sees it.
		 */
		if (rem6->id == id) {
			/* update the address */
			mptcp_debug("%s: updating old addr: %pI6 to addr %pI6 with id:%d\n",
				    __func__, &rem6->addr, addr, id);
			rem6->addr = *addr;
			rem6->port = port;
			mpcb->list_rcvd = 1;
			return 0;
		}
	}

	i = mptcp_find_free_index(mpcb->rem6_bits);
	/* Do we have already the maximum number of local/remote addresses? */
	if (i < 0) {
		mptcp_debug("%s: At max num of remote addresses: %d --- not adding address: %pI6\n",
			    __func__, MPTCP_MAX_ADDR, addr);
		return -1;
	}

	rem6 = &mpcb->remaddr6[i];

	/* Address is not known yet, store it */
	rem6->addr = *addr;
	rem6->port = port;
	rem6->bitfield = 0;
	rem6->retry_bitfield = 0;
	rem6->id = id;
	mpcb->list_rcvd = 1;
	mpcb->rem6_bits |= (1 << i);

	return 0;
}

/* Sets the bitfield of the remote-address field
 * local address is not set as it will disappear with the global address-list
 */
void mptcp_v6_set_init_addr_bit(struct mptcp_cb *mpcb,
				const struct in6_addr *daddr, u8 id)
{
	int i;
	mptcp_for_each_bit_set(mpcb->rem6_bits, i) {
		if (ipv6_addr_equal(&mpcb->remaddr6[i].addr, daddr)) {
			/* It's the initial flow - thus local index == 0 */
			mpcb->remaddr6[i].bitfield |= (1 << (id - MPTCP_MAX_ADDR));
			return;
		}
	}
}

int mptcp_v6_do_rcv(struct sock *meta_sk, struct sk_buff *skb)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *child, *rsk = NULL;
	int ret;

	if (!(TCP_SKB_CB(skb)->mptcp_flags & MPTCPHDR_JOIN)) {
		struct tcphdr *th = tcp_hdr(skb);
		const struct ipv6hdr *ip6h = ipv6_hdr(skb);
		struct sock *sk;

		sk = __inet6_lookup_established(sock_net(meta_sk),
						&tcp_hashinfo,
						&ip6h->saddr, th->source,
						&ip6h->daddr, ntohs(th->dest),
						inet6_iif(skb));

		if (!sk) {
			kfree_skb(skb);
			return 0;
		}
		if (is_meta_sk(sk)) {
			WARN("%s Did not find a sub-sk!\n", __func__);
			kfree_skb(skb);
			sock_put(sk);
			return 0;
		}

		if (sk->sk_state == TCP_TIME_WAIT) {
			inet_twsk_put(inet_twsk(sk));
			kfree_skb(skb);
			return 0;
		}

		ret = tcp_v6_do_rcv(sk, skb);
		sock_put(sk);

		return ret;
	}
	TCP_SKB_CB(skb)->mptcp_flags = 0;

	/* Has been removed from the tk-table. Thus, no new subflows.
	 *
	 * Check for close-state is necessary, because we may have been closed
	 * without passing by mptcp_close().
	 *
	 * When falling back, no new subflows are allowed either.
	 */
	if (meta_sk->sk_state == TCP_CLOSE || !tcp_sk(meta_sk)->inside_tk_table ||
	    mpcb->infinite_mapping_rcv || mpcb->send_infinite_mapping)
		goto reset_and_discard;

	child = tcp_v6_hnd_req(meta_sk, skb);

	if (!child)
		goto discard;

	if (child != meta_sk) {
		sock_rps_save_rxhash(child, skb);
		/* We don't call tcp_child_process here, because we hold
		 * already the meta-sk-lock and are sure that it is not owned
		 * by the user.
		 */
		ret = tcp_rcv_state_process(child, skb, tcp_hdr(skb), skb->len);
		bh_unlock_sock(child);
		sock_put(child);
		if (ret) {
			rsk = child;
			goto reset_and_discard;
		}
	} else {
		if (tcp_hdr(skb)->syn) {
			struct mp_join *join_opt = mptcp_find_join(skb);
			/* Currently we make two calls to mptcp_find_join(). This
			 * can probably be optimized. */
			if (mptcp_v6_add_raddress(mpcb,
						  (struct in6_addr *)&ipv6_hdr(skb)->saddr,
						  0,
						  join_opt->addr_id) < 0)
				goto reset_and_discard;
			mpcb->list_rcvd = 0;

			mptcp_v6_join_request(meta_sk, skb);
			goto discard;
		}
		goto reset_and_discard;
	}
	return 0;

reset_and_discard:
	if (reqsk_queue_len(&inet_csk(meta_sk)->icsk_accept_queue)) {
		const struct tcphdr *th = tcp_hdr(skb);
		struct request_sock **prev, *req;
		/* If we end up here, it means we should not have matched on the
		 * request-socket. But, because the request-sock queue is only
		 * destroyed in mptcp_close, the socket may actually already be
		 * in close-state (e.g., through shutdown()) while still having
		 * pending request sockets.
		 */
		req = inet6_csk_search_req(meta_sk, &prev, th->source,
					   &ipv6_hdr(skb)->saddr,
					   &ipv6_hdr(skb)->daddr, inet6_iif(skb));
		if (req) {
			inet_csk_reqsk_queue_unlink(meta_sk, req, prev);
			reqsk_queue_removed(&inet_csk(meta_sk)->icsk_accept_queue,
					    req);
			reqsk_free(req);
		}
	}

	tcp_v6_send_reset(rsk, skb);
discard:
	kfree_skb(skb);
	return 0;
}

/* After this, the ref count of the meta_sk associated with the request_sock
 * is incremented. Thus it is the responsibility of the caller
 * to call sock_put() when the reference is not needed anymore.
 */
struct sock *mptcp_v6_search_req(const __be16 rport, const struct in6_addr *raddr,
				 const struct in6_addr *laddr, const struct net *net)
{
	struct mptcp_request_sock *mtreq;
	struct sock *meta_sk = NULL;

	spin_lock(&mptcp_reqsk_hlock);
	list_for_each_entry(mtreq,
			    &mptcp_reqsk_htb[inet6_synq_hash(raddr, rport, 0,
							     MPTCP_HASH_SIZE)],
			    collide_tuple) {
		struct inet6_request_sock *treq = inet6_rsk(rev_mptcp_rsk(mtreq));
		meta_sk = mtreq->mpcb->meta_sk;

		if (inet_rsk(rev_mptcp_rsk(mtreq))->rmt_port == rport &&
		    rev_mptcp_rsk(mtreq)->rsk_ops->family == AF_INET6 &&
		    ipv6_addr_equal(&treq->rmt_addr, raddr) &&
		    ipv6_addr_equal(&treq->loc_addr, laddr) &&
		    net_eq(net, sock_net(meta_sk)))
			break;
		meta_sk = NULL;
	}

	if (meta_sk && unlikely(!atomic_inc_not_zero(&meta_sk->sk_refcnt)))
		meta_sk = NULL;
	spin_unlock(&mptcp_reqsk_hlock);

	return meta_sk;
}

/* Create a new IPv6 subflow.
 *
 * We are in user-context and meta-sock-lock is hold.
 */
int mptcp_init6_subsockets(struct sock *meta_sk, const struct mptcp_loc6 *loc,
			   struct mptcp_rem6 *rem)
{
	struct tcp_sock *tp;
	struct sock *sk;
	struct sockaddr_in6 loc_in, rem_in;
	struct socket sock;
	int ulid_size = 0, ret;

	/* Don't try again - even if it fails.
	 * There is a special case as the IPv6 address of the initial subflow
	 * has an id = 0. The other ones have id's in the range [8, 16[.
	 */
	rem->bitfield |= (1 << (loc->id - MPTCP_MAX_ADDR));

	/** First, create and prepare the new socket */

	sock.type = meta_sk->sk_socket->type;
	sock.state = SS_UNCONNECTED;
	sock.wq = meta_sk->sk_socket->wq;
	sock.file = meta_sk->sk_socket->file;
	sock.ops = NULL;

	ret = inet6_create(sock_net(meta_sk), &sock, IPPROTO_TCP, 1);
	if (unlikely(ret < 0)) {
		mptcp_debug("%s inet6_create failed ret: %d\n", __func__, ret);
		return ret;
	}

	sk = sock.sk;
	tp = tcp_sk(sk);

	/* All subsockets need the MPTCP-lock-class */
	lockdep_set_class_and_name(&(sk)->sk_lock.slock, &meta_slock_key, "slock-AF_INET-MPTCP");
	lockdep_init_map(&(sk)->sk_lock.dep_map, "sk_lock-AF_INET-MPTCP", &meta_key, 0);

	if (mptcp_add_sock(meta_sk, sk, loc->id, rem->id, GFP_KERNEL))
		goto error;

	tp->mptcp->slave_sk = 1;
	tp->mptcp->low_prio = loc->low_prio;

	/* Initializing the timer for an MPTCP subflow */
	setup_timer(&tp->mptcp->mptcp_ack_timer, mptcp_ack_handler, (unsigned long)sk);

	/** Then, connect the socket to the peer */

	ulid_size = sizeof(struct sockaddr_in6);
	loc_in.sin6_family = AF_INET6;
	rem_in.sin6_family = AF_INET6;
	loc_in.sin6_port = 0;
	if (rem->port)
		rem_in.sin6_port = rem->port;
	else
		rem_in.sin6_port = inet_sk(meta_sk)->inet_dport;
	loc_in.sin6_addr = loc->addr;
	rem_in.sin6_addr = rem->addr;

	ret = sock.ops->bind(&sock, (struct sockaddr *)&loc_in, ulid_size);
	if (ret < 0) {
		mptcp_debug("%s: MPTCP subsocket bind()failed, error %d\n",
			    __func__, ret);
		goto error;
	}

	mptcp_debug("%s: token %#x pi %d src_addr:%pI6:%d dst_addr:%pI6:%d\n",
		    __func__, tcp_sk(meta_sk)->mpcb->mptcp_loc_token,
		    tp->mptcp->path_index, &loc_in.sin6_addr,
		    ntohs(loc_in.sin6_port), &rem_in.sin6_addr,
		    ntohs(rem_in.sin6_port));

	ret = sock.ops->connect(&sock, (struct sockaddr *)&rem_in,
				ulid_size, O_NONBLOCK);
	if (ret < 0 && ret != -EINPROGRESS) {
		mptcp_debug("%s: MPTCP subsocket connect() failed, error %d\n",
			    __func__, ret);
		goto error;
	}

	sk_set_socket(sk, meta_sk->sk_socket);
	sk->sk_wq = meta_sk->sk_wq;

	return 0;

error:
	/* May happen if mptcp_add_sock fails first */
	if (!tp->mpc) {
		tcp_close(sk, 0);
	} else {
		local_bh_disable();
		mptcp_sub_force_close(sk);
		local_bh_enable();
	}
	return ret;
}
EXPORT_SYMBOL(mptcp_init6_subsockets);

int mptcp_pm_v6_init(void)
{
	int ret = 0;
	struct request_sock_ops *ops = &mptcp6_request_sock_ops;

	ops->slab_name = kasprintf(GFP_KERNEL, "request_sock_%s", "MPTCP6");
	if (ops->slab_name == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ops->slab = kmem_cache_create(ops->slab_name, ops->obj_size, 0,
				      SLAB_DESTROY_BY_RCU|SLAB_HWCACHE_ALIGN,
				      NULL);

	if (ops->slab == NULL) {
		ret =  -ENOMEM;
		goto err_reqsk_create;
	}

out:
	return ret;

err_reqsk_create:
	kfree(ops->slab_name);
	ops->slab_name = NULL;
	goto out;
}

void mptcp_pm_v6_undo(void)
{
	kmem_cache_destroy(mptcp6_request_sock_ops.slab);
	kfree(mptcp6_request_sock_ops.slab_name);
}
