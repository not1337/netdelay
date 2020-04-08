/*
 * This file is part of the netdelay project
 *
 * (C) 2020 Andreas Steinmetz, ast@domdv.de
 * The contents of this file is licensed under the GPL version 2 or, at
 * your choice, any later version of this license.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sched.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>

#define TXMINBUF	2048
#define RXMINBUF	256
#define TXRING		(64*64)
#define RXRING		64
#define RXTXBUF		2097152
#define DATASIZE	64

struct rxtx
{
	int fd;
	int head;
	union
	{
		int index;
		int tail;
	};
	int total;
	int size;
	int doff;
	int hoff;
	unsigned char *map;
	unsigned char *data[0];
};

static struct rxtx *rxopen(char *dev,int proto,int bpoll)
{
	int fd;
	int parm;
	int i;
	struct rxtx *rx;
	struct sockaddr_ll addr;
	struct tpacket_req req;

	if((fd=socket(AF_PACKET,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,
		htobe16(proto)))==-1)goto err1;
	memset(&addr,0,sizeof(addr));
	addr.sll_family=AF_PACKET;
	addr.sll_protocol=htobe16(proto);
	if(!(addr.sll_ifindex=if_nametoindex(dev)))goto err2;
	if(bind(fd,(struct sockaddr *)&addr,sizeof(struct sockaddr_ll)))
		goto err2;
	parm=TPACKET_V2;
	if(setsockopt(fd,SOL_PACKET,PACKET_VERSION,&parm,sizeof(parm)))
		goto err2;
#ifdef PACKET_IGNORE_OUTGOING
	parm=1;
	if(setsockopt(fd,SOL_PACKET,PACKET_IGNORE_OUTGOING,&parm,sizeof(parm)))
		goto err2;
#endif
	parm=RXTXBUF;
	if(setsockopt(fd,SOL_SOCKET,SO_RCVBUFFORCE,&parm,sizeof(parm)))
		goto err2;
	parm=TXMINBUF;
	if(setsockopt(fd,SOL_SOCKET,SO_SNDBUFFORCE,&parm,sizeof(parm)))
		goto err2;
	if(bpoll)if(setsockopt(fd,SOL_SOCKET,SO_BUSY_POLL,&bpoll,sizeof(bpoll)))
		goto err2;

	memset(&req,0,sizeof(req));
	req.tp_frame_size=TPACKET_ALIGN(TPACKET2_HDRLEN+ETH_HLEN)+
		TPACKET_ALIGN(DATASIZE);
	req.tp_block_size=sysconf(_SC_PAGESIZE);
	while(req.tp_block_size<req.tp_frame_size)req.tp_block_size<<=1;
	parm=req.tp_block_size/req.tp_frame_size;
	req.tp_block_nr=RXRING/parm;
	while(req.tp_block_nr*parm<RXRING)req.tp_block_nr++;
	req.tp_frame_nr=req.tp_block_nr*parm;
	if(setsockopt(fd,SOL_PACKET,PACKET_RX_RING,&req,sizeof(req)))goto err2;

	if(!(rx=malloc(sizeof(struct rxtx)+req.tp_frame_nr*sizeof(void *))))
		goto err2;
	rx->fd=fd;
	rx->index=0;
	rx->total=req.tp_frame_nr;
	rx->size=req.tp_block_nr*req.tp_block_size;
	rx->doff=TPACKET_ALIGN(TPACKET2_HDRLEN+ETH_HLEN);
	rx->hoff=rx->doff-ETH_HLEN;

	if(!(rx->map=mmap(NULL,rx->size,PROT_READ|PROT_WRITE,MAP_SHARED,
		rx->fd,0)))goto err3;

	for(i=0;i<req.tp_frame_nr;i++)
		rx->data[i]=rx->map+(i/parm)*req.tp_block_size+(i%parm)*
			req.tp_frame_size;

	return rx;

err3:	free(rx);
err2:	close(fd);
err1:	return NULL;
}

static void rxclose(struct rxtx *rx)
{
	munmap(rx->map,rx->size);
	close(rx->fd);
	free(rx);
}

static struct rxtx *txopen(char *dev)
{
	int fd;
	int parm;
	int i;
	struct rxtx *tx;
	struct sockaddr_ll addr;
	struct tpacket_req req;

	if((fd=socket(AF_PACKET,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,0))==-1)
		goto err1;
	memset(&addr,0,sizeof(addr));
	addr.sll_family=AF_PACKET;
	addr.sll_protocol=0;
	if(!(addr.sll_ifindex=if_nametoindex(dev)))goto err2;
	if(bind(fd,(struct sockaddr *)&addr,sizeof(struct sockaddr_ll)))
		goto err2;
	parm=TPACKET_V2;
	if(setsockopt(fd,SOL_PACKET,PACKET_VERSION,&parm,sizeof(parm)))
		goto err2;
	parm=1;
	if(setsockopt(fd,SOL_PACKET,PACKET_LOSS,&parm,sizeof(parm)))goto err2;
	if(setsockopt(fd,SOL_PACKET,PACKET_QDISC_BYPASS,&parm,sizeof(parm)))
		goto err2;

	parm=RXMINBUF;
	if(setsockopt(fd,SOL_SOCKET,SO_RCVBUFFORCE,&parm,sizeof(parm)))
		goto err2;
	parm=RXTXBUF;
	if(setsockopt(fd,SOL_SOCKET,SO_SNDBUFFORCE,&parm,sizeof(parm)))
		goto err2;

	memset(&req,0,sizeof(req));
	req.tp_frame_size=TPACKET_ALIGN(TPACKET2_HDRLEN)+
		TPACKET_ALIGN(DATASIZE);
	req.tp_block_size=sysconf(_SC_PAGESIZE);
	while(req.tp_block_size<req.tp_frame_size)req.tp_block_size<<=1;
	parm=req.tp_block_size/req.tp_frame_size;
	req.tp_block_nr=TXRING/parm;
	while(req.tp_block_nr*parm<TXRING)req.tp_block_nr++;
	req.tp_frame_nr=req.tp_block_nr*parm;
	if(setsockopt(fd,SOL_PACKET,PACKET_TX_RING,&req,sizeof(req)))goto err2;

	if(!(tx=malloc(sizeof(struct rxtx)+req.tp_frame_nr*sizeof(void *))))
		goto err2;
	tx->fd=fd;
	tx->head=0;
	tx->tail=0;
	tx->total=req.tp_frame_nr;
	tx->size=req.tp_block_nr*req.tp_block_size;
	tx->hoff=TPACKET2_HDRLEN-sizeof(struct sockaddr_ll);
	tx->doff=tx->hoff+ETH_HLEN;

	if(!(tx->map=mmap(NULL,tx->size,PROT_READ|PROT_WRITE,MAP_SHARED,
		tx->fd,0)))goto err3;

	for(i=0;i<req.tp_frame_nr;i++)
		tx->data[i]=tx->map+(i/parm)*req.tp_block_size+(i%parm)*
			req.tp_frame_size;

	return tx;

err3:	free(tx);
err2:	close(fd);
err1:	return NULL;
}

static void txclose(struct rxtx *tx)
{
	munmap(tx->map,tx->size);
	close(tx->fd);
	free(tx);
}

static int mksock(int family,int proto,int port,char *dev,int dscp,int prio,
	int cpu,int bpoll)
{
	int s;
	int i;
	struct sockaddr_in a;
	struct sockaddr_in6 a6;

	if((s=socket(family,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,
		proto?IPPROTO_UDPLITE:0))==-1)goto err1;
	i=1;
	if(setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&i,sizeof(i)))
		goto err2;
	if(bpoll)if(setsockopt(s,SOL_SOCKET,SO_BUSY_POLL,&bpoll,sizeof(bpoll)))
		goto err2;

	if(cpu!=-1)if(setsockopt(s,SOL_SOCKET,SO_INCOMING_CPU,&cpu,sizeof(cpu)))
		goto err2;

	if(family==AF_INET)
	{
		memset(&a,0,sizeof(a));
		a.sin_family=family;
		a.sin_port=htobe16(port);
		a.sin_addr.s_addr=INADDR_ANY;
		if(bind(s,(struct sockaddr *)(&a),sizeof(a)))goto err2;
	}
	else
	{
		i=0;
		if(setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&i,sizeof(i)))
			goto err2;
		memset(&a6,0,sizeof(a6));
		a6.sin6_family=family;
		a6.sin6_port=htobe16(port);
		a6.sin6_addr=in6addr_any;
		if(bind(s,(struct sockaddr *)(&a6),sizeof(a6)))goto err2;
	}
	if(dev)
	{
		if(setsockopt(s,SOL_SOCKET,SO_BINDTODEVICE,dev,strlen(dev)))
			goto err2;
		i=1;
		if(setsockopt(s,SOL_SOCKET,SO_DONTROUTE,&i,sizeof(i)))
			goto err2;
	}
	i=1048576;
	if(setsockopt(s,SOL_SOCKET,SO_RCVBUFFORCE,&i,sizeof(i)))
		goto err2;
	if(setsockopt(s,SOL_SOCKET,SO_SNDBUFFORCE,&i,sizeof(i)))
		goto err2;
	/* IP_TOS messes with SO_PRIORITY setup, so must come first and then
	   always set SO_PRIORITY afterwards */
	if(dscp)
	{
		i=(dscp<<2)&0xfc;
		if(family==AF_INET6)
			if(setsockopt(s,IPPROTO_IPV6,IPV6_TCLASS,&i,sizeof(i)))
				goto err2;
		if(setsockopt(s,IPPROTO_IP,IP_TOS,&i,sizeof(i)))goto err2;
		if(setsockopt(s,SOL_SOCKET,SO_PRIORITY,&prio,sizeof(prio)))
			goto err2;
	}
	else if(prio)if(setsockopt(s,SOL_SOCKET,SO_PRIORITY,&prio,sizeof(prio)))
		goto err2;
	return s;

err2:	close(s);
err1:	return -1;
}

static void l2initiator(struct rxtx *tx,struct rxtx *rx,void *src,void *dst,
	int prio,int vid,int ts)
{
	struct tpacket2_hdr *rxhdr;
	struct tpacket2_hdr *txhdr;
	struct ethhdr *txe;
	struct timespec *data;
	int curr;
	int next;
	int pre=20;
	int rep;
	uint64_t val;
	uint64_t min=-1;
	uint64_t max=0;
	uint64_t sum=0;
	uint64_t n=0;
	struct pollfd p;
	struct timespec tm;
	uint16_t vdata[2];
	struct tm stm;
	char datim[64];

	p.fd=rx->fd;
	p.events=POLLIN;

	vdata[0]=htobe16((prio<<13)|(vid&0xfff));
	vdata[1]=htobe16(ETH_P_802_EX1);

	while(1)
	{
		curr=tx->head;
		next=tx->head+1;
		if(next==tx->total)next=0;

		while(tx->tail!=curr)
		{
			txhdr=(struct tpacket2_hdr *)tx->data[tx->tail];
			switch(txhdr->tp_status)
			{
			case TP_STATUS_WRONG_FORMAT:
				txhdr->tp_status=TP_STATUS_AVAILABLE;
			case TP_STATUS_AVAILABLE:
				if((tx->tail+=1)==tx->total)tx->tail=0;
				continue;
			}
			break;
		}

		txhdr=(struct tpacket2_hdr *)tx->data[curr];
		switch(txhdr->tp_status)
		{
		case TP_STATUS_WRONG_FORMAT:
			txhdr->tp_status=TP_STATUS_AVAILABLE;
		case TP_STATUS_AVAILABLE:
			break;
		default:fprintf(stderr,"transmit queue overflow\n");
			return;
		}

		txe=(struct ethhdr *)(tx->data[curr]+tx->hoff);
		memcpy(txe->h_source,src,ETH_ALEN);
		memcpy(txe->h_dest,dst,ETH_ALEN);

		if(prio)
		{
			txe->h_proto=htobe16(ETH_P_8021Q);
			memcpy(tx->data[curr]+tx->doff,vdata,4);
			clock_gettime(CLOCK_MONOTONIC_RAW,
				(void *)(tx->data[curr]+tx->doff+4));
		}
		else
		{
			txe->h_proto=htobe16(ETH_P_802_EX1);
			clock_gettime(CLOCK_MONOTONIC_RAW,
				(void *)(tx->data[curr]+tx->doff));
		}

		txhdr->tp_len=DATASIZE;
		txhdr->tp_status=TP_STATUS_SEND_REQUEST;
		tx->head=next;
		rep=5;
again:		if(send(tx->fd,NULL,0,MSG_DONTWAIT)<0)
		{
			if(errno==ENOBUFS)
			{
				if(rep--)
				{
					usleep(20);
					goto again;
				}
				perror("Warning: send");
				goto skip;
			}
			perror("send\n");
			return;
		}

		if(poll(&p,1,1000)<1)
		{
			fprintf(stderr,"Warning: poll timed out\n");
			goto skip;
		}

		clock_gettime(CLOCK_MONOTONIC_RAW,&tm);

		if(!(p.revents&POLLIN))
		{
			fprintf(stderr,"Warning: no data after poll\n");
			goto skip;
		}

		rxhdr=(struct tpacket2_hdr *)rx->data[rx->index];
		if(!(rxhdr->tp_status&TP_STATUS_USER))return;
		data=(struct timespec *)(rx->data[rx->index]+rx->doff);

		if(tm.tv_nsec<data->tv_nsec)
		{
			tm.tv_nsec+=1000000000;
			tm.tv_sec--;
		}
		if(tm.tv_sec<data->tv_sec)
		{
			fprintf(stderr,"time mismatch, aborting\n");
			return;
		}
		tm.tv_sec-=data->tv_sec;
		tm.tv_nsec-=data->tv_nsec;

		rxhdr->tp_status=TP_STATUS_KERNEL;
		if((rx->index+=1)==rx->total)rx->index=0;

		if(tm.tv_sec)fprintf(stderr, "Warning: wrong data skipped\n");
		else if(pre)pre--;
		else
		{
			val=tm.tv_sec;
			val*=1000000000;
			val+=tm.tv_nsec;
			sum+=val;
			n++;
			if(val<min)min=val;
			if(val>max)max=val;

			if(!(n&0xf))
			{
				if(ts)
				{
					clock_gettime(CLOCK_REALTIME,&tm);
					localtime_r(&tm.tv_sec,&stm);
					strftime(datim,sizeof(datim),"%T",&stm);
					sprintf(datim+8,".%09lu ",tm.tv_nsec);
				}
				else *datim=0;
				printf("%s%llu %llu %llu\n",datim,
					(unsigned long long)min,
					(unsigned long long)(sum/n),
					(unsigned long long)max);
			}
		}

skip:		usleep(50000);
	}
}

static void l2responder(struct rxtx *rx,struct rxtx *tx,int prio,int vid)
{
	struct pollfd p;
	void *data;
	struct tpacket2_hdr *rxhdr;
	struct tpacket2_hdr *txhdr;
	struct ethhdr *rxe;
	struct ethhdr *txe;
	int curr;
	int next;
	int rep;
	uint16_t vdata[2];

	p.fd=rx->fd;
	p.events=POLLIN;

	vdata[0]=htobe16((prio<<13)|(vid&0xfff));

	while(1)
	{
		if(poll(&p,1,-1)<1)continue;
		if(!(p.revents&POLLIN))continue;

		while(1)
		{
			rxhdr=(struct tpacket2_hdr *)rx->data[rx->index];
			if(!(rxhdr->tp_status&TP_STATUS_USER))break;
			data=(struct l2msg *)(rx->data[rx->index]+rx->doff);

			curr=tx->head;
			next=tx->head+1;
			if(next==tx->total)next=0;

			while(tx->tail!=curr)
			{
				txhdr=(struct tpacket2_hdr *)tx->data[tx->tail];
				switch(txhdr->tp_status)
				{
				case TP_STATUS_WRONG_FORMAT:
					txhdr->tp_status=TP_STATUS_AVAILABLE;
				case TP_STATUS_AVAILABLE:
					if((tx->tail+=1)==tx->total)tx->tail=0;
					continue;
				}
				break;
			}

			txhdr=(struct tpacket2_hdr *)tx->data[curr];
			switch(txhdr->tp_status)
			{
			case TP_STATUS_WRONG_FORMAT:
				txhdr->tp_status=TP_STATUS_AVAILABLE;
			case TP_STATUS_AVAILABLE:
				break;
			default:fprintf(stderr, "Warning: tx queue full\n");
				goto skip;
			}

			rxe=(struct ethhdr *)(rx->data[rx->index]+rx->hoff);
			txe=(struct ethhdr *)(tx->data[curr]+tx->hoff);

			memcpy(txe->h_source,rxe->h_dest,ETH_ALEN);
			memcpy(txe->h_dest,rxe->h_source,ETH_ALEN);

			if(prio)
			{
				txe->h_proto=htobe16(ETH_P_8021Q);
				vdata[1]=rxe->h_proto;
				memcpy(tx->data[curr]+tx->doff,vdata,4);
				memcpy(tx->data[curr]+tx->doff+4,data,16);
			}
			else
			{
				txe->h_proto=rxe->h_proto;
				memcpy(tx->data[curr]+tx->doff,data,16);
			}
			txhdr->tp_len=DATASIZE;
			txhdr->tp_status=TP_STATUS_SEND_REQUEST;

			rep=5;
again:			if(send(tx->fd,NULL,0,MSG_DONTWAIT)<0)
			{
				if(errno==ENOBUFS)if(rep--)
				{
					usleep(20);
					goto again;
				}
				perror("Warning: send");
			}

			tx->head=next;

skip:			rxhdr->tp_status=TP_STATUS_KERNEL;
			if((rx->index+=1)==rx->total)rx->index=0;
		}
	}
}

static void udpinitiator(int us,int port,struct sockaddr_storage *ss,int ts)
{
	int l;
	struct sockaddr_in *s4=(struct sockaddr_in *)ss;
	struct sockaddr_in6 *s6=(struct sockaddr_in6 *)ss;
	struct timespec *data;
	int pre=20;
	uint64_t val;
	uint64_t min=-1;
	uint64_t max=0;
	uint64_t sum=0;
	uint64_t n=0;
	struct pollfd p;
	struct timespec tm;
	unsigned char bfr[DATASIZE];
	struct tm stm;
	char datim[64];

	if(ss->ss_family==AF_INET)s4->sin_port=htobe16(port);
	else s6->sin6_port=htobe16(port);

	p.fd=us;
	p.events=POLLIN|POLLHUP|POLLERR;

	data=(struct timespec *)bfr;

	while(1)
	{
		clock_gettime(CLOCK_MONOTONIC_RAW,data);
		if((l=sendto(us,bfr,sizeof(bfr),MSG_DONTWAIT,
			(struct sockaddr *)ss,
			sizeof(struct sockaddr_storage)))!=sizeof(bfr))
		{
			if(l<0)perror("Warning: sendto");
			else fprintf(stderr,"Warning: sendto unspecified "
				"error");
			goto skip;
		}

		if(poll(&p,1,1000)<1)
		{
			fprintf(stderr,"Warning: poll timed out\n");
			goto skip;
		}

		clock_gettime(CLOCK_MONOTONIC_RAW,&tm);

		if(!(p.revents&POLLIN))
		{
			fprintf(stderr,"Warning: no data after poll\n");
			goto skip;
		}

		if((l=recv(us,bfr,sizeof(bfr),MSG_DONTWAIT))<=0)
		{
			if(l<0)perror("recv");
			else fprintf(stderr,"unspecified receive error\n");
			return;
		}

		if(l!=DATASIZE)
		{
			fprintf(stderr,"Warning: unexpected data length\n");
			goto skip;
		}

		if(tm.tv_nsec<data->tv_nsec)
		{
			tm.tv_nsec+=1000000000;
			tm.tv_sec--;
		}
		if(tm.tv_sec<data->tv_sec)
		{
			fprintf(stderr,"time mismatch, aborting\n");
			return;
		}
		tm.tv_sec-=data->tv_sec;
		tm.tv_nsec-=data->tv_nsec;

		if(tm.tv_sec)fprintf(stderr, "Warning: wrong data skipped\n");
		else if(pre)pre--;
		else
		{
			val=tm.tv_sec;
			val*=1000000000;
			val+=tm.tv_nsec;
			sum+=val;
			n++;
			if(val<min)min=val;
			if(val>max)max=val;

			if(!(n&0xf))
			{
				if(ts)
				{
					clock_gettime(CLOCK_REALTIME,&tm);
					localtime_r(&tm.tv_sec,&stm);
					strftime(datim,sizeof(datim),"%T",&stm);
					sprintf(datim+8,".%09lu ",tm.tv_nsec);
				}
				else *datim=0;
				printf("%s%llu %llu %llu\n",datim,
					(unsigned long long)min,
					(unsigned long long)(sum/n),
					(unsigned long long)max);
			}
		}

skip:		usleep(50000);
	}
}

static void udpresponder(int us)
{
	int l;
	socklen_t sl;
	struct pollfd p;
	struct sockaddr_storage ss;
	struct sockaddr_in *s4=(struct sockaddr_in *)&ss;
	struct sockaddr_in6 *s6=(struct sockaddr_in6 *)&ss;
	struct sockaddr_in tmp;
	unsigned char bfr[DATASIZE];

	p.fd=us;
	p.events=POLLIN|POLLHUP|POLLERR;

	memset(&tmp,0,sizeof(tmp));
	tmp.sin_family=AF_INET;

	while(1)
	{
		if(poll(&p,1,-1)<1)continue;
		if(p.revents&(POLLHUP|POLLERR))
		{
			fprintf(stderr,"socket error\n");
			return;
		}
		if(!(p.revents&POLLIN))continue;
		sl=sizeof(struct sockaddr_storage);
		if((l=recvfrom(us,bfr,sizeof(bfr),MSG_DONTWAIT,
			(struct sockaddr *)&ss,&sl))<=0)
		{
			if(l<0)perror("recvfrom");
			else fprintf(stderr,"unspecified receive error\n");
			return;
		}

		if(l!=DATASIZE)
		{
			fprintf(stderr,"Warning: unexpected data length\n");
			continue;
		}

		if(ss.ss_family==AF_INET6)if(!memcmp(s6->sin6_addr.s6_addr,
			"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xff",12))
		{
			memcpy(&tmp.sin_addr.s_addr,s6->sin6_addr.s6_addr+12,4);
			tmp.sin_port=s6->sin6_port;
			*s4=tmp;
		}

		if((l=sendto(us,bfr,sizeof(bfr),MSG_DONTWAIT,
			(struct sockaddr *)&ss,sizeof(ss)))!=sizeof(bfr))
		{
			if(l<0)perror("Warning: sendto");
			else fprintf(stderr,"Warning: unspecified sendto "
				"error\n");
		}
	}
}

static int mac2bin(char *mac,unsigned char *hwaddr)
{
	int i;

	for(i=0;i<ETH_ALEN;i++,mac+=3,hwaddr++)
	{
		switch(i)
		{
		case ETH_ALEN-1:
			if(mac[2])return -1;
			break;
		default:if(mac[2]!=':')return -1;
			break;
		}
		if(mac[0]>='0'&&mac[0]<='9')*hwaddr=mac[0]-'0';
		else if(mac[0]>='A'&&mac[0]<='F')*hwaddr=mac[0]-'A'+10;
		else if(mac[0]>='a'&&mac[0]<='f')*hwaddr=mac[0]-'a'+10;
		else return -1;
		*hwaddr<<=4;
		if(mac[1]>='0'&&mac[1]<='9')*hwaddr|=mac[1]-'0';
		else if(mac[1]>='A'&&mac[1]<='F')*hwaddr|=mac[1]-'A'+10;
		else if(mac[1]>='a'&&mac[1]<='f')*hwaddr|=mac[1]-'a'+10;
		else return -1;
	}
	return 0;
}

static int getmac(char *dev,unsigned char *addr)
{
	int s;
	struct ifreq ifreq;

	if((s=socket(AF_INET,SOCK_DGRAM,0))==-1)return -1;
	memset(&ifreq,0,sizeof(ifreq));
	strncpy(ifreq.ifr_name,dev,sizeof(ifreq.ifr_name)-1);
	if(ioctl(s,SIOCGIFHWADDR,&ifreq))
	{
		close(s);
		return -1;
	}
	close(s);
	if(ifreq.ifr_hwaddr.sa_family!=ARPHRD_ETHER)return -1;
	memcpy(addr,ifreq.ifr_hwaddr.sa_data,ETH_ALEN);
	return 0;
}

static int getaddr(char *addr,struct sockaddr_storage *dest,int v4)
{
	int e;
	struct hostent r;
	struct hostent *res;
	struct sockaddr_in *a4=(struct sockaddr_in *)dest;
	struct sockaddr_in6 *a6=(struct sockaddr_in6 *)dest;
	char bfr[8192];

	memset(dest,0,sizeof(struct sockaddr_storage));

	if(inet_pton(AF_INET,addr,&a4->sin_addr.s_addr)==1)goto chk4;
	if(!v4)if(inet_pton(AF_INET6,addr,a6->sin6_addr.s6_addr)==1)goto chk6;
	if(!v4)if(!gethostbyname2_r(addr,AF_INET6,&r,bfr,sizeof(bfr),&res,&e))
		if(r.h_addrtype==AF_INET6)
	{
		memcpy(a6->sin6_addr.s6_addr,r.h_addr,16);
		goto chk6;
	}
	if(!gethostbyname2_r(addr,AF_INET,&r,bfr,sizeof(bfr),&res,&e))
		if(r.h_addrtype==AF_INET)
	{
		memcpy(&a4->sin_addr.s_addr,r.h_addr,4);
		goto chk4;
	}
	return -1;

chk4:	if(!memcmp(&a4->sin_addr.s_addr,"\x00\x00\x00\x00",4))return -1;
	if(be32toh(a4->sin_addr.s_addr)>=0xe0000000)return -1;
	a4->sin_family=AF_INET;
	return 0;

chk6:	if(memcmp(a6->sin6_addr.s6_addr,"\xfe\x80\x00\x00\x00\x00\x00\x00",8)&&
		(a6->sin6_addr.s6_addr[0]&0xfe)!=0xfc&&
		(a6->sin6_addr.s6_addr[0]&0xe0)!=0x20)return -1;
	a6->sin6_family=AF_INET6;
	return 0;
}

static int chkaddr(struct sockaddr_storage *ss,int local)
{
	if(ss->ss_family!=AF_INET6)return 0;
	if(((struct sockaddr_in6 *)ss)->sin6_addr.s6_addr[0]==0xfe)
	{
		if(!local)return -1;
	}
	else if(local)return -1;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,"Usage:\n\n"
	"netdelay [<options>] -R -i <netdevice>\n"
	"netdelay [<options>] -I -i <netdevice> -d <destination-mac>\n"
	"netdelay [<options>] -R -u|-U -P <port>\n"
	"netdelay [<options>] -I -u|-U -h <destination-address> -P <port>\n\n"
	"-I initiator mode\n"
	"-R responder mode\n"
	"-u use UDP instead of layer 2\n"
	"-U use UDPLITE instead of layer 2\n"
	"-4 force IPv4 for UDP/UDPLITE\n"
	"-b <value> set busy poll (1-500)\n"
	"-i <netdevice> network device to use\n"
	"-d <destination-mac> ethernet address of responder\n"
	"-h <destination-host> UDP/UDPLITE destination host\n"
	"-P <port> UDP/UDPLITE local and remote port (1-65535)\n"
	"-D <value> set DSCP value for UDP/UDPLITE (1-63)\n"
	"-r <value> set realtime priority (1-99)\n"
	"-c <value> set core to run on (0-1023)\n"
	"-v <value> set 802.1q vlan (1-4094)\n"
	"-p <value> set 802.1p priority (1-7)\n"
	"-l <value> set system latency via /dev/cpu_dma_latency (0-9999)\n\n"
	"-m lock process memory\n"
	"-t print timestamp\n"
	"If UDP/UDPLITE is used specifying a network device disables IPv4\n"
	"routing and requires an IPv6 link local address.\n\n"
	"This tool measures network roundtrip delay with layer 2 packets\n"
	"bypassing the kernel network stack.\n\n"
	"The output is 3 columns, all in nanoseconds:\n\n"
	"minimum-delay average-delay maximum-delay\n");
	exit(1);
}

int main(int argc,char *argv[])
{
	int c;
	int mode=0;
	int prio=0;
	int vid=0;
	int rt=0;
	int32_t lat=-1;
	int cpu=-1;
	int udp=0;
	int us=-1;
	int fd=-1;
	int port=0;
	int dscp=0;
	int v4=0;
	int bpoll=0;
	int mla=0;
	int ts=0;
	char *host=NULL;
	char *dev=NULL;
	char *dmac=NULL;
	struct rxtx *tx=NULL;
	struct rxtx *rx=NULL;
	struct sched_param prm;
	cpu_set_t core;
	struct sockaddr_storage ss;
	unsigned char src[ETH_ALEN];
	unsigned char dst[ETH_ALEN];

	while((c=getopt(argc,argv,"IRi:d:r:c:p:l:h:P:uUD:4b:mt"))!=-1)switch(c)
	{
	case 'I':
		mode=2;
		break;

	case 'R':
		mode=1;
		break;

	case 'i':
		dev=optarg;
		if(getmac(dev,src))usage();
		break;

	case 'd':
		dmac=optarg;
		if(mac2bin(dmac,dst))usage();
		break;

	case 'r':
		if((rt=atoi(optarg))<1||rt>99)usage();
		break;

	case 'c':
		if((cpu=atoi(optarg))<0||cpu>1023)usage();
		break;

	case 'v':
		if((vid=atoi(optarg))<1||vid>4094)usage();
		break;

	case 'p':
		if((prio=atoi(optarg))<1||prio>7)usage();
		break;

	case 'l':
		if((lat=atoi(optarg))<0||lat>9999)usage();
		break;

	case 'h':
		host=optarg;
		break;

	case 'P':
		if((port=atoi(optarg))<1||port>65535)usage();
		break;

	case 'u':
		udp=1;
		break;

	case 'U':
		udp=2;
		break;

	case 'D':
		if((dscp=atoi(optarg))<1||dscp>63)usage();
		break;

	case '4':
		v4=1;
		break;

	case 'b':
		if((bpoll=atoi(optarg))<1||bpoll>500)usage();
		break;

	case 'm':
		mla=1;
		break;

	case 't':
		ts=1;
		break;

	default:usage();
	}

	if(udp)
	{
		ss.ss_family=(v4?AF_INET:AF_INET6);
		switch(mode)
		{
		case 2:	if(!host||getaddr(host,&ss,v4))usage();
			if(chkaddr(&ss,dev?1:0))usage();
		case 1:	if(port)break;
		default:usage();
		}
	}
	else
	{
		switch(mode)
		{
		case 2:	if(!dmac)usage();
		case 1:	if(dev)break;
		default:usage();
		}
	}

	if(mla)if(mlockall(MCL_CURRENT|MCL_FUTURE))
	{
		perror("mlockall");
		return 1;
	}

	if(cpu!=-1)
	{
		CPU_ZERO(&core);
		CPU_SET(cpu,&core);
		if(sched_setaffinity(0,sizeof(cpu_set_t),&core))
		{
			perror("sched_setaffinity");
			return 1;
		}
	}

	if(udp)
	{
		us=mksock(ss.ss_family,udp-1,port,dev,dscp,prio,cpu,bpoll);
	}
	else
	{
		if(!(tx=txopen(dev)))goto txerr;
		if(!(rx=rxopen(dev,ETH_P_802_EX1,bpoll)))
		{
			txclose(tx);
txerr:			fprintf(stderr,"Cannot access %s\n",dev);
			return 1;
		}
	}

	if(rt)
	{
		prm.sched_priority=rt;
		if(sched_setscheduler(0,SCHED_RR,&prm))
		{
			perror("sched_setscheduler");
			rxclose(rx);
			txclose(tx);
			return 1;
		}
	}

	if(lat!=-1)
	{
		if((fd=open("/dev/cpu_dma_latency",O_WRONLY|O_CLOEXEC))==-1)
		{
			perror("open");
			rxclose(rx);
			txclose(tx);
			return 1;
		}
		if(write(fd,&lat,sizeof(lat))!=sizeof(lat))
		{
			perror("write");
			rxclose(rx);
			txclose(tx);
			close(fd);
			return 1;
		}
	}

	if(udp)
	{
		if(mode==2)udpinitiator(us,port,&ss,ts);
		else udpresponder(us);
	}
	else
	{
		if(mode==2)l2initiator(tx,rx,src,dst,prio,vid,ts);
		else l2responder(rx,tx,prio,vid);
	}

	if(fd!=-1)close(fd);
	if(us!=-1)close(us);
	if(rx)rxclose(rx);
	if(tx)txclose(tx);

	return 1;
}
