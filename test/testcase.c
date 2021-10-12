OURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/capability.h>
#include <linux/genetlink.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/in6.h>
#include <linux/loop.h>
#include <linux/neighbour.h>
#include <linux/net.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rfkill.h>
#include <linux/rtnetlink.h>
#include <linux/veth.h>

static unsigned long long procid;

static void sleep_ms(uint64_t ms)
{
	  usleep(ms * 1000);
}

static uint64_t current_time_ms(void)
{
	  struct timespec ts;
	    if (clock_gettime(CLOCK_MONOTONIC, &ts))
		        exit(1);
	      return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool write_file(const char* file, const char* what, ...)
{
	  char buf[1024];
	    va_list args;
	      va_start(args, what);
	        vsnprintf(buf, sizeof(buf), what, args);
		  va_end(args);
		    buf[sizeof(buf) - 1] = 0;
		      int len = strlen(buf);
		        int fd = open(file, O_WRONLY | O_CLOEXEC);
			  if (fd == -1)
				      return false;
			    if (write(fd, buf, len) != len) {
				        int err = errno;
					    close(fd);
					        errno = err;
						    return false;
						      }
			      close(fd);
			        return true;
}

struct nlmsg {
	  char* pos;
	    int nesting;
	      struct nlattr* nested[8];
	        char buf[4096];
};

static void netlink_init(struct nlmsg* nlmsg, int typ, int flags,
		                         const void* data, int size)
{
	  memset(nlmsg, 0, sizeof(*nlmsg));
	    struct nlmsghdr* hdr = (struct nlmsghdr*)nlmsg->buf;
	      hdr->nlmsg_type = typ;
	        hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags;
		  memcpy(hdr + 1, data, size);
		    nlmsg->pos = (char*)(hdr + 1) + NLMSG_ALIGN(size);
}

static void netlink_attr(struct nlmsg* nlmsg, int typ, const void* data,
		                         int size)
{
	  struct nlattr* attr = (struct nlattr*)nlmsg->pos;
	    attr->nla_len = sizeof(*attr) + size;
	      attr->nla_type = typ;
	        if (size > 0)
			    memcpy(attr + 1, data, size);
		  nlmsg->pos += NLMSG_ALIGN(attr->nla_len);
}

static int netlink_send_ext(struct nlmsg* nlmsg, int sock, uint16_t reply_type,
		                            int* reply_len, bool dofail)
{
	  if (nlmsg->pos > nlmsg->buf + sizeof(nlmsg->buf) || nlmsg->nesting)
		      exit(1);
	    struct nlmsghdr* hdr = (struct nlmsghdr*)nlmsg->buf;
	      hdr->nlmsg_len = nlmsg->pos - nlmsg->buf;
	        struct sockaddr_nl addr;
		  memset(&addr, 0, sizeof(addr));
		    addr.nl_family = AF_NETLINK;
		      ssize_t n = sendto(sock, nlmsg->buf, hdr->nlmsg_len, 0,
				                           (struct sockaddr*)&addr, sizeof(addr));
		        if (n != (ssize_t)hdr->nlmsg_len) {
				    if (dofail)
					          exit(1);
				        return -1;
					  }
			  n = recv(sock, nlmsg->buf, sizeof(nlmsg->buf), 0);
			    if (reply_len)
				        *reply_len = 0;
			      if (n < 0) {
				          if (dofail)
						        exit(1);
					      return -1;
					        }
			        if (n < (ssize_t)sizeof(struct nlmsghdr)) {
					    errno = EINVAL;
					        if (dofail)
							      exit(1);
						    return -1;
						      }
				  if (hdr->nlmsg_type == NLMSG_DONE)
					      return 0;
				    if (reply_len && hdr->nlmsg_type == reply_type) {
					        *reply_len = n;
						    return 0;
						      }
				      if (n < (ssize_t)(sizeof(struct nlmsghdr) + sizeof(struct nlmsgerr))) {
					          errno = EINVAL;
						      if (dofail)
							            exit(1);
						          return -1;
							    }
				        if (hdr->nlmsg_type != NLMSG_ERROR) {
						    errno = EINVAL;
						        if (dofail)
								      exit(1);
							    return -1;
							      }
					  errno = -((struct nlmsgerr*)(hdr + 1))->error;
					    return -errno;
}

static int netlink_send(struct nlmsg* nlmsg, int sock)
{
	  return netlink_send_ext(nlmsg, sock, 0, NULL, true);
}

static int netlink_query_family_id(struct nlmsg* nlmsg, int sock,
		                                   const char* family_name, bool dofail)
{
	  struct genlmsghdr genlhdr;
	    memset(&genlhdr, 0, sizeof(genlhdr));
	      genlhdr.cmd = CTRL_CMD_GETFAMILY;
	        netlink_init(nlmsg, GENL_ID_CTRL, 0, &genlhdr, sizeof(genlhdr));
		  netlink_attr(nlmsg, CTRL_ATTR_FAMILY_NAME, family_name,
				                 strnlen(family_name, GENL_NAMSIZ - 1) + 1);
		    int n = 0;
		      int err = netlink_send_ext(nlmsg, sock, GENL_ID_CTRL, &n, dofail);
		        if (err < 0) {
				    return -1;
				      }
			  uint16_t id = 0;
			    struct nlattr* attr = (struct nlattr*)(nlmsg->buf + NLMSG_HDRLEN +
					                                             NLMSG_ALIGN(sizeof(genlhdr)));
			      for (; (char*)attr < nlmsg->buf + n;
					             attr = (struct nlattr*)((char*)attr + NLMSG_ALIGN(attr->nla_len))) {
				          if (attr->nla_type == CTRL_ATTR_FAMILY_ID) {
						        id = *(uint16_t*)(attr + 1);
							      break;
							          }
					    }
			        if (!id) {
					    errno = EINVAL;
					        return -1;
						  }
				  recv(sock, nlmsg->buf, sizeof(nlmsg->buf), 0);
				    return id;
}

static struct nlmsg nlmsg;

#define WIFI_INITIAL_DEVICE_COUNT 2
#define WIFI_MAC_BASE                                                          \
	  {                                                                            \
		      0x08, 0x02, 0x11, 0x00, 0x00, 0x00                                         \
		    }
#define WIFI_IBSS_BSSID                                                        \
	  {                                                                            \
		      0x50, 0x50, 0x50, 0x50, 0x50, 0x50                                         \
		    }
#define WIFI_IBSS_SSID                                                         \
	  {                                                                            \
		      0x10, 0x10, 0x10, 0x10, 0x10, 0x10                                         \
		    }
#define WIFI_DEFAULT_FREQUENCY 2412
#define WIFI_DEFAULT_SIGNAL 0
#define WIFI_DEFAULT_RX_RATE 1
#define HWSIM_CMD_REGISTER 1
#define HWSIM_CMD_FRAME 2
#define HWSIM_CMD_NEW_RADIO 4
#define HWSIM_ATTR_SUPPORT_P2P_DEVICE 14
#define HWSIM_ATTR_PERM_ADDR 22

#define IF_OPER_UP 6
struct join_ibss_props {
	  int wiphy_freq;
	    bool wiphy_freq_fixed;
	      uint8_t* mac;
	        uint8_t* ssid;
		  int ssid_len;
};

static int set_interface_state(const char* interface_name, int on)
{
	  struct ifreq ifr;
	    int sock = socket(AF_INET, SOCK_DGRAM, 0);
	      if (sock < 0) {
		          return -1;
			    }
	        memset(&ifr, 0, sizeof(ifr));
		  strcpy(ifr.ifr_name, interface_name);
		    int ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
		      if (ret < 0) {
			          close(sock);
				      return -1;
				        }
		        if (on)
				    ifr.ifr_flags |= IFF_UP;
			  else
				      ifr.ifr_flags &= ~IFF_UP;
			    ret = ioctl(sock, SIOCSIFFLAGS, &ifr);
			      close(sock);
			        if (ret < 0) {
					    return -1;
					      }
				  return 0;
}

static int nl80211_set_interface(struct nlmsg* nlmsg, int sock,
		                                 int nl80211_family, uint32_t ifindex,
						                                  uint32_t iftype)
{
	  struct genlmsghdr genlhdr;
	    memset(&genlhdr, 0, sizeof(genlhdr));
	      genlhdr.cmd = NL80211_CMD_SET_INTERFACE;
	        netlink_init(nlmsg, nl80211_family, 0, &genlhdr, sizeof(genlhdr));
		  netlink_attr(nlmsg, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
		    netlink_attr(nlmsg, NL80211_ATTR_IFTYPE, &iftype, sizeof(iftype));
		      int err = netlink_send(nlmsg, sock);
		        if (err < 0) {
				  }
			  return err;
}

static int nl80211_join_ibss(struct nlmsg* nlmsg, int sock, int nl80211_family,
		                             uint32_t ifindex, struct join_ibss_props* props)
{
	  struct genlmsghdr genlhdr;
	    memset(&genlhdr, 0, sizeof(genlhdr));
	      genlhdr.cmd = NL80211_CMD_JOIN_IBSS;
	        netlink_init(nlmsg, nl80211_family, 0, &genlhdr, sizeof(genlhdr));
		  netlink_attr(nlmsg, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
		    netlink_attr(nlmsg, NL80211_ATTR_SSID, props->ssid, props->ssid_len);
		      netlink_attr(nlmsg, NL80211_ATTR_WIPHY_FREQ, &(props->wiphy_freq),
				                     sizeof(props->wiphy_freq));
		        if (props->mac)
				    netlink_attr(nlmsg, NL80211_ATTR_MAC, props->mac, ETH_ALEN);
			  if (props->wiphy_freq_fixed)
				      netlink_attr(nlmsg, NL80211_ATTR_FREQ_FIXED, NULL, 0);
			    int err = netlink_send(nlmsg, sock);
			      if (err < 0) {
				        }
			        return err;
}

static int get_ifla_operstate(struct nlmsg* nlmsg, int ifindex)
{
	  struct ifinfomsg info;
	    memset(&info, 0, sizeof(info));
	      info.ifi_family = AF_UNSPEC;
	        info.ifi_index = ifindex;
		  int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
		    if (sock == -1) {
			        return -1;
				  }
		      netlink_init(nlmsg, RTM_GETLINK, 0, &info, sizeof(info));
		        int n;
			  int err = netlink_send_ext(nlmsg, sock, RTM_NEWLINK, &n, true);
			    close(sock);
			      if (err) {
				          return -1;
					    }
			        struct rtattr* attr = IFLA_RTA(NLMSG_DATA(nlmsg->buf));
				  for (; RTA_OK(attr, n); attr = RTA_NEXT(attr, n)) {
					      if (attr->rta_type == IFLA_OPERSTATE)
						            return *((int32_t*)RTA_DATA(attr));
					        }
				    return -1;
}

static int await_ifla_operstate(struct nlmsg* nlmsg, char* interface,
		                                int operstate)
{
	  int ifindex = if_nametoindex(interface);
	    while (true) {
		        usleep(1000);
			    int ret = get_ifla_operstate(nlmsg, ifindex);
			        if (ret < 0)
					      return ret;
				    if (ret == operstate)
					          return 0;
				      }
	      return 0;
}

static int nl80211_setup_ibss_interface(struct nlmsg* nlmsg, int sock,
		                                        int nl80211_family_id, char* interface,
							                                        struct join_ibss_props* ibss_props)
{
	  int ifindex = if_nametoindex(interface);
	    if (ifindex == 0) {
		        return -1;
			  }
	      int ret = nl80211_set_interface(nlmsg, sock, nl80211_family_id, ifindex,
			                                        NL80211_IFTYPE_ADHOC);
	        if (ret < 0) {
			    return -1;
			      }
		  ret = set_interface_state(interface, 1);
		    if (ret < 0) {
			        return -1;
				  }
		      ret = nl80211_join_ibss(nlmsg, sock, nl80211_family_id, ifindex, ibss_props);
		        if (ret < 0) {
				    return -1;
				      }
			  return 0;
}

static int hwsim80211_create_device(struct nlmsg* nlmsg, int sock,
		                                    int hwsim_family,
						                                        uint8_t mac_addr[ETH_ALEN])
{
	  struct genlmsghdr genlhdr;
	    memset(&genlhdr, 0, sizeof(genlhdr));
	      genlhdr.cmd = HWSIM_CMD_NEW_RADIO;
	        netlink_init(nlmsg, hwsim_family, 0, &genlhdr, sizeof(genlhdr));
		  netlink_attr(nlmsg, HWSIM_ATTR_SUPPORT_P2P_DEVICE, NULL, 0);
		    netlink_attr(nlmsg, HWSIM_ATTR_PERM_ADDR, mac_addr, ETH_ALEN);
		      int err = netlink_send(nlmsg, sock);
		        if (err < 0) {
				  }
			  return err;
}

static void initialize_wifi_devices(void)
{
	  int rfkill = open("/dev/rfkill", O_RDWR);
	    if (rfkill == -1) {
		        if (errno != ENOENT && errno != EACCES)
				      exit(1);
			  } else {
				      struct rfkill_event event = {0};
				          event.type = RFKILL_TYPE_ALL;
					      event.op = RFKILL_OP_CHANGE_ALL;
					          if (write(rfkill, &event, sizeof(event)) != (ssize_t)(sizeof(event)))
							        exit(1);
						      close(rfkill);
						        }
	      uint8_t mac_addr[6] = WIFI_MAC_BASE;
	        int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
		  if (sock < 0) {
			      return;
			        }
		    int hwsim_family_id =
			          netlink_query_family_id(&nlmsg, sock, "MAC80211_HWSIM", true);
		      int nl80211_family_id =
			            netlink_query_family_id(&nlmsg, sock, "nl80211", true);
		        uint8_t ssid[] = WIFI_IBSS_SSID;
			  uint8_t bssid[] = WIFI_IBSS_BSSID;
			    struct join_ibss_props ibss_props = {.wiphy_freq = WIFI_DEFAULT_FREQUENCY,
				                                           .wiphy_freq_fixed = true,
									                                          .mac = bssid,
														                                         .ssid = ssid,
																			                                        .ssid_len = sizeof(ssid)};
			      for (int device_id = 0; device_id < WIFI_INITIAL_DEVICE_COUNT; device_id++) {
				          mac_addr[5] = device_id;
					      int ret = hwsim80211_create_device(&nlmsg, sock, hwsim_family_id, mac_addr);
					          if (ret < 0)
							        exit(1);
						      char interface[6] = "wlan0";
						          interface[4] += device_id;
							      if (nl80211_setup_ibss_interface(&nlmsg, sock, nl80211_family_id, interface,
										                                           &ibss_props) < 0)
								            exit(1);
							        }
			        for (int device_id = 0; device_id < WIFI_INITIAL_DEVICE_COUNT; device_id++) {
					    char interface[6] = "wlan0";
					        interface[4] += device_id;
						    int ret = await_ifla_operstate(&nlmsg, interface, IF_OPER_UP);
						        if (ret < 0)
								      exit(1);
							  }
				  close(sock);
}

#define MAX_FDS 30

static long syz_open_dev(volatile long a0, volatile long a1, volatile long a2)
{
	  if (a0 == 0xc || a0 == 0xb) {
		      char buf[128];
		          sprintf(buf, "/dev/%s/%d:%d", a0 == 0xc ? "char" : "block", (uint8_t)a1,
					              (uint8_t)a2);
			      return open(buf, O_RDWR, 0);
			        } else {
					    char buf[1024];
					        char* hash;
						    strncpy(buf, (char*)a0, sizeof(buf) - 1);
						        buf[sizeof(buf) - 1] = 0;
							    while ((hash = strchr(buf, '#'))) {
								          *hash = '0' + (char)(a1 % 10);
									        a1 /= 10;
										    }
							        return open(buf, a2, 0);
								  }
}

struct fs_image_segment {
	  void* data;
	    uintptr_t size;
	      uintptr_t offset;
};

#define IMAGE_MAX_SEGMENTS 4096
#define IMAGE_MAX_SIZE (129 << 20)

#define sys_memfd_create 319

static unsigned long fs_image_segment_check(unsigned long size,
		                                            unsigned long nsegs,
							                                                struct fs_image_segment* segs)
{
	  if (nsegs > IMAGE_MAX_SEGMENTS)
		      nsegs = IMAGE_MAX_SEGMENTS;
	    for (size_t i = 0; i < nsegs; i++) {
		        if (segs[i].size > IMAGE_MAX_SIZE)
				      segs[i].size = IMAGE_MAX_SIZE;
			    segs[i].offset %= IMAGE_MAX_SIZE;
			        if (segs[i].offset > IMAGE_MAX_SIZE - segs[i].size)
					      segs[i].offset = IMAGE_MAX_SIZE - segs[i].size;
				    if (size < segs[i].offset + segs[i].offset)
					          size = segs[i].offset + segs[i].offset;
				      }
	      if (size > IMAGE_MAX_SIZE)
		          size = IMAGE_MAX_SIZE;
	        return size;
}
static int setup_loop_device(long unsigned size, long unsigned nsegs,
		                             struct fs_image_segment* segs,
					                                  const char* loopname, int* memfd_p, int* loopfd_p)
{
	  int err = 0, loopfd = -1;
	    size = fs_image_segment_check(size, nsegs, segs);
	      int memfd = syscall(sys_memfd_create, "syzkaller", 0);
	        if (memfd == -1) {
			    err = errno;
			        goto error;
				  }
		  if (ftruncate(memfd, size)) {
			      err = errno;
			          goto error_close_memfd;
				    }
		    for (size_t i = 0; i < nsegs; i++) {
			        if (pwrite(memfd, segs[i].data, segs[i].size, segs[i].offset) < 0) {
					    }
				  }
		      loopfd = open(loopname, O_RDWR);
		        if (loopfd == -1) {
				    err = errno;
				        goto error_close_memfd;
					  }
			  if (ioctl(loopfd, LOOP_SET_FD, memfd)) {
				      if (errno != EBUSY) {
					            err = errno;
						          goto error_close_loop;
							      }
				          ioctl(loopfd, LOOP_CLR_FD, 0);
					      usleep(1000);
					          if (ioctl(loopfd, LOOP_SET_FD, memfd)) {
							        err = errno;
								      goto error_close_loop;
								          }
						    }
			    *memfd_p = memfd;
			      *loopfd_p = loopfd;
			        return 0;

error_close_loop:
				  close(loopfd);
error_close_memfd:
				    close(memfd);
error:
				      errno = err;
				        return -1;
}

static long syz_read_part_table(volatile unsigned long size,
		                                volatile unsigned long nsegs,
						                                volatile long segments)
{
	  struct fs_image_segment* segs = (struct fs_image_segment*)segments;
	    int err = 0, res = -1, loopfd = -1, memfd = -1;
	      char loopname[64];
	        snprintf(loopname, sizeof(loopname), "/dev/loop%llu", procid);
		  if (setup_loop_device(size, nsegs, segs, loopname, &memfd, &loopfd) == -1)
			      return -1;
		    struct loop_info64 info;
		      if (ioctl(loopfd, LOOP_GET_STATUS64, &info)) {
			          err = errno;
				      goto error_clear_loop;
				        }
		        info.lo_flags |= LO_FLAGS_PARTSCAN;
			  if (ioctl(loopfd, LOOP_SET_STATUS64, &info)) {
				      err = errno;
				          goto error_clear_loop;
					    }
			    res = 0;
			      for (unsigned long i = 1, j = 0; i < 8; i++) {
				          snprintf(loopname, sizeof(loopname), "/dev/loop%llup%d", procid, (int)i);
					      struct stat statbuf;
					          if (stat(loopname, &statbuf) == 0) {
							        char linkname[64];
								      snprintf(linkname, sizeof(linkname), "./file%d", (int)j++);
								            if (symlink(loopname, linkname)) {
										          }
									        }
						    }
error_clear_loop:
			        ioctl(loopfd, LOOP_CLR_FD, 0);
				  close(loopfd);
				    close(memfd);
				      errno = err;
				        return res;
}

static void setup_common()
{
	  if (mount(0, "/sys/fs/fuse/connections", "fusectl", 0, 0)) {
		    }
}

static void loop();

static void sandbox_common()
{
	  prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
	    setsid();
	      struct rlimit rlim;
	        rlim.rlim_cur = rlim.rlim_max = (200 << 20);
		  setrlimit(RLIMIT_AS, &rlim);
		    rlim.rlim_cur = rlim.rlim_max = 32 << 20;
		      setrlimit(RLIMIT_MEMLOCK, &rlim);
		        rlim.rlim_cur = rlim.rlim_max = 136 << 20;
			  setrlimit(RLIMIT_FSIZE, &rlim);
			    rlim.rlim_cur = rlim.rlim_max = 1 << 20;
			      setrlimit(RLIMIT_STACK, &rlim);
			        rlim.rlim_cur = rlim.rlim_max = 0;
				  setrlimit(RLIMIT_CORE, &rlim);
				    rlim.rlim_cur = rlim.rlim_max = 256;
				      setrlimit(RLIMIT_NOFILE, &rlim);
				        if (unshare(CLONE_NEWNS)) {
						  }
					  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
						    }
					    if (unshare(CLONE_NEWIPC)) {
						      }
					      if (unshare(0x02000000)) {
						        }
					        if (unshare(CLONE_NEWUTS)) {
							  }
						  if (unshare(CLONE_SYSVSEM)) {
							    }
						    typedef struct {
							        const char* name;
								    const char* value;
								      } sysctl_t;
						      static const sysctl_t sysctls[] = {
							            {"/proc/sys/kernel/shmmax", "16777216"},
								          {"/proc/sys/kernel/shmall", "536870912"},
									        {"/proc/sys/kernel/shmmni", "1024"},
										      {"/proc/sys/kernel/msgmax", "8192"},
										            {"/proc/sys/kernel/msgmni", "1024"},
											          {"/proc/sys/kernel/msgmnb", "1024"},
												        {"/proc/sys/kernel/sem", "1024 1048576 500 1024"},
													  };
						        unsigned i;
							  for (i = 0; i < sizeof(sysctls) / sizeof(sysctls[0]); i++)
								      write_file(sysctls[i].name, sysctls[i].value);
}

static int wait_for_loop(int pid)
{
	  if (pid < 0)
		      exit(1);
	    int status = 0;
	      while (waitpid(-1, &status, __WALL) != pid) {
		        }
	        return WEXITSTATUS(status);
}

static void drop_caps(void)
{
	  struct __user_cap_header_struct cap_hdr = {};
	    struct __user_cap_data_struct cap_data[2] = {};
	      cap_hdr.version = _LINUX_CAPABILITY_VERSION_3;
	        cap_hdr.pid = getpid();
		  if (syscall(SYS_capget, &cap_hdr, &cap_data))
			      exit(1);
		    const int drop = (1 << CAP_SYS_PTRACE) | (1 << CAP_SYS_NICE);
		      cap_data[0].effective &= ~drop;
		        cap_data[0].permitted &= ~drop;
			  cap_data[0].inheritable &= ~drop;
			    if (syscall(SYS_capset, &cap_hdr, &cap_data))
				        exit(1);
}

static int do_sandbox_none(void)
{
	  if (unshare(CLONE_NEWPID)) {
		    }
	    int pid = fork();
	      if (pid != 0)
		          return wait_for_loop(pid);
	        setup_common();
		  sandbox_common();
		    drop_caps();
		      if (unshare(CLONE_NEWNET)) {
			        }
		        initialize_wifi_devices();
			  loop();
			    exit(1);
}

static void kill_and_wait(int pid, int* status)
{
	  kill(-pid, SIGKILL);
	    kill(pid, SIGKILL);
	      for (int i = 0; i < 100; i++) {
		          if (waitpid(-1, status, WNOHANG | __WALL) == pid)
				        return;
			      usleep(1000);
			        }
	        DIR* dir = opendir("/sys/fs/fuse/connections");
		  if (dir) {
			      for (;;) {
				            struct dirent* ent = readdir(dir);
					          if (!ent)
							          break;
						        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
								        continue;
							      char abort[300];
							            snprintf(abort, sizeof(abort), "/sys/fs/fuse/connections/%s/abort",
										                   ent->d_name);
								          int fd = open(abort, O_WRONLY);
									        if (fd == -1) {
											        continue;
												      }
										      if (write(fd, abort, 1) < 0) {
											            }
										            close(fd);
											        }
			          closedir(dir);
				    } else {
					      }
		    while (waitpid(-1, status, __WALL) != pid) {
			      }
}

static void reset_loop()
{
	  char buf[64];
	    snprintf(buf, sizeof(buf), "/dev/loop%llu", procid);
	      int loopfd = open(buf, O_RDWR);
	        if (loopfd != -1) {
			    ioctl(loopfd, LOOP_CLR_FD, 0);
			        close(loopfd);
				  }
}

static void setup_test()
{
	  prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
	    setpgrp();
	      write_file("/proc/self/oom_score_adj", "1000");
}

static void close_fds()
{
	  for (int fd = 3; fd < MAX_FDS; fd++)
		      close(fd);
}

static void execute_one(void);

#define WAIT_FLAGS __WALL

static void loop(void)
{
	  int iter = 0;
	    for (;; iter++) {
		        reset_loop();
			    int pid = fork();
			        if (pid < 0)
					      exit(1);
				    if (pid == 0) {
					          setup_test();
						        execute_one();
							      close_fds();
							            exit(0);
								        }
				        int status = 0;
					    uint64_t start = current_time_ms();
					        for (;;) {
							      if (waitpid(-1, &status, WNOHANG | WAIT_FLAGS) == pid)
								              break;
							            sleep_ms(1);
								          if (current_time_ms() - start < 5000) {
										          continue;
											        }
									        kill_and_wait(pid, &status);
										      break;
										          }
						  }
}

uint64_t r[1] = {0xffffffffffffffff};

void execute_one(void)
{
	  intptr_t res = 0;
	    memcpy((void*)0x20000080, "/dev/loop#\000", 11);
	      res = -1;
	        res = syz_open_dev(0x20000080, 0, 0);
		  if (res != -1)
			      r[0] = res;
		    syscall(__NR_sysinfo, 0x200007c0ul);
		      *(uint32_t*)0x20000240 = 1;
		        *(uint32_t*)0x20000244 = 0;
			  *(uint32_t*)0x20000248 = 0;
			    *(uint64_t*)0x20000250 = 0x20000800;
			      syscall(__NR_ioctl, r[0], 0x1269, 0x20000240ul);
			        syz_read_part_table(0, 0, 0);
}
int main(void)
{
	  syscall(__NR_mmap, 0x1ffff000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
	    syscall(__NR_mmap, 0x20000000ul, 0x1000000ul, 7ul, 0x32ul, -1, 0ul);
	      syscall(__NR_mmap, 0x21000000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
	        for (procid = 0; procid < 6; procid++) {
			    if (fork() == 0) {
				          do_sandbox_none();
					      }
			      }
		  sleep(1000000);
		    return 0
}
