#define _GNU_SOURCE

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/capability.h>
#include <linux/genetlink.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/in6.h>
#include <linux/neighbour.h>
#include <linux/net.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/rfkill.h>
#include <linux/rtnetlink.h>
#include <linux/usb/ch9.h>
#include <linux/veth.h>

static unsigned long long procid;

static __thread int skip_segv;
static __thread jmp_buf segv_env;

static void segv_handler(int sig, siginfo_t* info, void* ctx)
{
  uintptr_t addr = (uintptr_t)info->si_addr;
  const uintptr_t prog_start = 1 << 20;
  const uintptr_t prog_end = 100 << 20;
  int skip = __atomic_load_n(&skip_segv, __ATOMIC_RELAXED) != 0;
  int valid = addr < prog_start || addr > prog_end;
  if (skip && valid) {
    _longjmp(segv_env, 1);
  }
  exit(sig);
}

static void install_segv_handler(void)
{
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  syscall(SYS_rt_sigaction, 0x20, &sa, NULL, 8);
  syscall(SYS_rt_sigaction, 0x21, &sa, NULL, 8);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = segv_handler;
  sa.sa_flags = SA_NODEFER | SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}

#define NONFAILING(...)                                                        \
  ({                                                                           \
    int ok = 1;                                                                \
    __atomic_fetch_add(&skip_segv, 1, __ATOMIC_SEQ_CST);                       \
    if (_setjmp(segv_env) == 0) {                                              \
      __VA_ARGS__;                                                             \
    } else                                                                     \
      ok = 0;                                                                  \
    __atomic_fetch_sub(&skip_segv, 1, __ATOMIC_SEQ_CST);                       \
    ok;                                                                        \
  })

static void sleep_ms(uint64_t ms)
{
  usleep(ms * 1000);
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

static void netlink_nest(struct nlmsg* nlmsg, int typ)
{
  struct nlattr* attr = (struct nlattr*)nlmsg->pos;
  attr->nla_type = typ;
  nlmsg->pos += sizeof(*attr);
  nlmsg->nested[nlmsg->nesting++] = attr;
}

static void netlink_done(struct nlmsg* nlmsg)
{
  struct nlattr* attr = nlmsg->nested[--nlmsg->nesting];
  attr->nla_len = nlmsg->pos - (char*)attr;
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

static void netlink_add_device_impl(struct nlmsg* nlmsg, const char* type,
                                    const char* name)
{
  struct ifinfomsg hdr;
  memset(&hdr, 0, sizeof(hdr));
  netlink_init(nlmsg, RTM_NEWLINK, NLM_F_EXCL | NLM_F_CREATE, &hdr,
               sizeof(hdr));
  if (name)
    netlink_attr(nlmsg, IFLA_IFNAME, name, strlen(name));
  netlink_nest(nlmsg, IFLA_LINKINFO);
  netlink_attr(nlmsg, IFLA_INFO_KIND, type, strlen(type));
}

static void netlink_device_change(struct nlmsg* nlmsg, int sock,
                                  const char* name, bool up, const char* master,
                                  const void* mac, int macsize,
                                  const char* new_name)
{
  struct ifinfomsg hdr;
  memset(&hdr, 0, sizeof(hdr));
  if (up)
    hdr.ifi_flags = hdr.ifi_change = IFF_UP;
  hdr.ifi_index = if_nametoindex(name);
  netlink_init(nlmsg, RTM_NEWLINK, 0, &hdr, sizeof(hdr));
  if (new_name)
    netlink_attr(nlmsg, IFLA_IFNAME, new_name, strlen(new_name));
  if (master) {
    int ifindex = if_nametoindex(master);
    netlink_attr(nlmsg, IFLA_MASTER, &ifindex, sizeof(ifindex));
  }
  if (macsize)
    netlink_attr(nlmsg, IFLA_ADDRESS, mac, macsize);
  int err = netlink_send(nlmsg, sock);
  if (err < 0) {
  }
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

#define USB_MAX_IFACE_NUM 4
#define USB_MAX_EP_NUM 32
#define USB_MAX_FDS 6

struct usb_endpoint_index {
  struct usb_endpoint_descriptor desc;
  int handle;
};

struct usb_iface_index {
  struct usb_interface_descriptor* iface;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bInterfaceClass;
  struct usb_endpoint_index eps[USB_MAX_EP_NUM];
  int eps_num;
};

struct usb_device_index {
  struct usb_device_descriptor* dev;
  struct usb_config_descriptor* config;
  uint8_t bDeviceClass;
  uint8_t bMaxPower;
  int config_length;
  struct usb_iface_index ifaces[USB_MAX_IFACE_NUM];
  int ifaces_num;
  int iface_cur;
};

struct usb_info {
  int fd;
  struct usb_device_index index;
};

static struct usb_info usb_devices[USB_MAX_FDS];
static int usb_devices_num;

static bool parse_usb_descriptor(const char* buffer, size_t length,
                                 struct usb_device_index* index)
{
  if (length < sizeof(*index->dev) + sizeof(*index->config))
    return false;
  memset(index, 0, sizeof(*index));
  index->dev = (struct usb_device_descriptor*)buffer;
  index->config = (struct usb_config_descriptor*)(buffer + sizeof(*index->dev));
  index->bDeviceClass = index->dev->bDeviceClass;
  index->bMaxPower = index->config->bMaxPower;
  index->config_length = length - sizeof(*index->dev);
  index->iface_cur = -1;
  size_t offset = 0;
  while (true) {
    if (offset + 1 >= length)
      break;
    uint8_t desc_length = buffer[offset];
    uint8_t desc_type = buffer[offset + 1];
    if (desc_length <= 2)
      break;
    if (offset + desc_length > length)
      break;
    if (desc_type == USB_DT_INTERFACE &&
        index->ifaces_num < USB_MAX_IFACE_NUM) {
      struct usb_interface_descriptor* iface =
          (struct usb_interface_descriptor*)(buffer + offset);
      index->ifaces[index->ifaces_num].iface = iface;
      index->ifaces[index->ifaces_num].bInterfaceNumber =
          iface->bInterfaceNumber;
      index->ifaces[index->ifaces_num].bAlternateSetting =
          iface->bAlternateSetting;
      index->ifaces[index->ifaces_num].bInterfaceClass = iface->bInterfaceClass;
      index->ifaces_num++;
    }
    if (desc_type == USB_DT_ENDPOINT && index->ifaces_num > 0) {
      struct usb_iface_index* iface = &index->ifaces[index->ifaces_num - 1];
      if (iface->eps_num < USB_MAX_EP_NUM) {
        memcpy(&iface->eps[iface->eps_num].desc, buffer + offset,
               sizeof(iface->eps[iface->eps_num].desc));
        iface->eps_num++;
      }
    }
    offset += desc_length;
  }
  return true;
}

static struct usb_device_index* add_usb_index(int fd, const char* dev,
                                              size_t dev_len)
{
  int i = __atomic_fetch_add(&usb_devices_num, 1, __ATOMIC_RELAXED);
  if (i >= USB_MAX_FDS)
    return NULL;
  if (!parse_usb_descriptor(dev, dev_len, &usb_devices[i].index))
    return NULL;
  __atomic_store_n(&usb_devices[i].fd, fd, __ATOMIC_RELEASE);
  return &usb_devices[i].index;
}

static struct usb_device_index* lookup_usb_index(int fd)
{
  for (int i = 0; i < USB_MAX_FDS; i++) {
    if (__atomic_load_n(&usb_devices[i].fd, __ATOMIC_ACQUIRE) == fd) {
      return &usb_devices[i].index;
    }
  }
  return NULL;
}

struct vusb_connect_string_descriptor {
  uint32_t len;
  char* str;
} __attribute__((packed));

struct vusb_connect_descriptors {
  uint32_t qual_len;
  char* qual;
  uint32_t bos_len;
  char* bos;
  uint32_t strs_len;
  struct vusb_connect_string_descriptor strs[0];
} __attribute__((packed));

static const char default_string[] = {8, USB_DT_STRING, 's', 0, 'y', 0, 'z', 0};

static const char default_lang_id[] = {4, USB_DT_STRING, 0x09, 0x04};

static bool
lookup_connect_response_in(int fd, const struct vusb_connect_descriptors* descs,
                           const struct usb_ctrlrequest* ctrl,
                           char** response_data, uint32_t* response_length)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  uint8_t str_idx;
  if (!index)
    return false;
  switch (ctrl->bRequestType & USB_TYPE_MASK) {
  case USB_TYPE_STANDARD:
    switch (ctrl->bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
      switch (ctrl->wValue >> 8) {
      case USB_DT_DEVICE:
        *response_data = (char*)index->dev;
        *response_length = sizeof(*index->dev);
        return true;
      case USB_DT_CONFIG:
        *response_data = (char*)index->config;
        *response_length = index->config_length;
        return true;
      case USB_DT_STRING:
        str_idx = (uint8_t)ctrl->wValue;
        if (descs && str_idx < descs->strs_len) {
          *response_data = descs->strs[str_idx].str;
          *response_length = descs->strs[str_idx].len;
          return true;
        }
        if (str_idx == 0) {
          *response_data = (char*)&default_lang_id[0];
          *response_length = default_lang_id[0];
          return true;
        }
        *response_data = (char*)&default_string[0];
        *response_length = default_string[0];
        return true;
      case USB_DT_BOS:
        *response_data = descs->bos;
        *response_length = descs->bos_len;
        return true;
      case USB_DT_DEVICE_QUALIFIER:
        if (!descs->qual) {
          struct usb_qualifier_descriptor* qual =
              (struct usb_qualifier_descriptor*)response_data;
          qual->bLength = sizeof(*qual);
          qual->bDescriptorType = USB_DT_DEVICE_QUALIFIER;
          qual->bcdUSB = index->dev->bcdUSB;
          qual->bDeviceClass = index->dev->bDeviceClass;
          qual->bDeviceSubClass = index->dev->bDeviceSubClass;
          qual->bDeviceProtocol = index->dev->bDeviceProtocol;
          qual->bMaxPacketSize0 = index->dev->bMaxPacketSize0;
          qual->bNumConfigurations = index->dev->bNumConfigurations;
          qual->bRESERVED = 0;
          *response_length = sizeof(*qual);
          return true;
        }
        *response_data = descs->qual;
        *response_length = descs->qual_len;
        return true;
      default:
        break;
      }
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return false;
}

typedef bool (*lookup_connect_out_response_t)(
    int fd, const struct vusb_connect_descriptors* descs,
    const struct usb_ctrlrequest* ctrl, bool* done);

static bool lookup_connect_response_out_generic(
    int fd, const struct vusb_connect_descriptors* descs,
    const struct usb_ctrlrequest* ctrl, bool* done)
{
  switch (ctrl->bRequestType & USB_TYPE_MASK) {
  case USB_TYPE_STANDARD:
    switch (ctrl->bRequest) {
    case USB_REQ_SET_CONFIGURATION:
      *done = true;
      return true;
    default:
      break;
    }
    break;
  }
  return false;
}

struct vusb_descriptor {
  uint8_t req_type;
  uint8_t desc_type;
  uint32_t len;
  char data[0];
} __attribute__((packed));

struct vusb_descriptors {
  uint32_t len;
  struct vusb_descriptor* generic;
  struct vusb_descriptor* descs[0];
} __attribute__((packed));

struct vusb_response {
  uint8_t type;
  uint8_t req;
  uint32_t len;
  char data[0];
} __attribute__((packed));

struct vusb_responses {
  uint32_t len;
  struct vusb_response* generic;
  struct vusb_response* resps[0];
} __attribute__((packed));

static bool lookup_control_response(const struct vusb_descriptors* descs,
                                    const struct vusb_responses* resps,
                                    struct usb_ctrlrequest* ctrl,
                                    char** response_data,
                                    uint32_t* response_length)
{
  int descs_num = 0;
  int resps_num = 0;
  if (descs)
    descs_num = (descs->len - offsetof(struct vusb_descriptors, descs)) /
                sizeof(descs->descs[0]);
  if (resps)
    resps_num = (resps->len - offsetof(struct vusb_responses, resps)) /
                sizeof(resps->resps[0]);
  uint8_t req = ctrl->bRequest;
  uint8_t req_type = ctrl->bRequestType & USB_TYPE_MASK;
  uint8_t desc_type = ctrl->wValue >> 8;
  if (req == USB_REQ_GET_DESCRIPTOR) {
    int i;
    for (i = 0; i < descs_num; i++) {
      struct vusb_descriptor* desc = descs->descs[i];
      if (!desc)
        continue;
      if (desc->req_type == req_type && desc->desc_type == desc_type) {
        *response_length = desc->len;
        if (*response_length != 0)
          *response_data = &desc->data[0];
        else
          *response_data = NULL;
        return true;
      }
    }
    if (descs && descs->generic) {
      *response_data = &descs->generic->data[0];
      *response_length = descs->generic->len;
      return true;
    }
  } else {
    int i;
    for (i = 0; i < resps_num; i++) {
      struct vusb_response* resp = resps->resps[i];
      if (!resp)
        continue;
      if (resp->type == req_type && resp->req == req) {
        *response_length = resp->len;
        if (*response_length != 0)
          *response_data = &resp->data[0];
        else
          *response_data = NULL;
        return true;
      }
    }
    if (resps && resps->generic) {
      *response_data = &resps->generic->data[0];
      *response_length = resps->generic->len;
      return true;
    }
  }
  return false;
}

#define UDC_NAME_LENGTH_MAX 128

struct usb_raw_init {
  __u8 driver_name[UDC_NAME_LENGTH_MAX];
  __u8 device_name[UDC_NAME_LENGTH_MAX];
  __u8 speed;
};

enum usb_raw_event_type {
  USB_RAW_EVENT_INVALID = 0,
  USB_RAW_EVENT_CONNECT = 1,
  USB_RAW_EVENT_CONTROL = 2,
};

struct usb_raw_event {
  __u32 type;
  __u32 length;
  __u8 data[0];
};

struct usb_raw_ep_io {
  __u16 ep;
  __u16 flags;
  __u32 length;
  __u8 data[0];
};

#define USB_RAW_EPS_NUM_MAX 30
#define USB_RAW_EP_NAME_MAX 16
#define USB_RAW_EP_ADDR_ANY 0xff

struct usb_raw_ep_caps {
  __u32 type_control : 1;
  __u32 type_iso : 1;
  __u32 type_bulk : 1;
  __u32 type_int : 1;
  __u32 dir_in : 1;
  __u32 dir_out : 1;
};

struct usb_raw_ep_limits {
  __u16 maxpacket_limit;
  __u16 max_streams;
  __u32 reserved;
};

struct usb_raw_ep_info {
  __u8 name[USB_RAW_EP_NAME_MAX];
  __u32 addr;
  struct usb_raw_ep_caps caps;
  struct usb_raw_ep_limits limits;
};

struct usb_raw_eps_info {
  struct usb_raw_ep_info eps[USB_RAW_EPS_NUM_MAX];
};

#define USB_RAW_IOCTL_INIT _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_ENABLE _IOW('U', 5, struct usb_endpoint_descriptor)
#define USB_RAW_IOCTL_EP_DISABLE _IOW('U', 6, __u32)
#define USB_RAW_IOCTL_EP_WRITE _IOW('U', 7, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP_READ _IOWR('U', 8, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EPS_INFO _IOR('U', 11, struct usb_raw_eps_info)
#define USB_RAW_IOCTL_EP0_STALL _IO('U', 12)
#define USB_RAW_IOCTL_EP_SET_HALT _IOW('U', 13, __u32)
#define USB_RAW_IOCTL_EP_CLEAR_HALT _IOW('U', 14, __u32)
#define USB_RAW_IOCTL_EP_SET_WEDGE _IOW('U', 15, __u32)

static int usb_raw_open()
{
  return open("/dev/raw-gadget", O_RDWR);
}

static int usb_raw_init(int fd, uint32_t speed, const char* driver,
                        const char* device)
{
  struct usb_raw_init arg;
  strncpy((char*)&arg.driver_name[0], driver, sizeof(arg.driver_name));
  strncpy((char*)&arg.device_name[0], device, sizeof(arg.device_name));
  arg.speed = speed;
  return ioctl(fd, USB_RAW_IOCTL_INIT, &arg);
}

static int usb_raw_run(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_RUN, 0);
}

static int usb_raw_event_fetch(int fd, struct usb_raw_event* event)
{
  return ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, event);
}

static int usb_raw_ep0_write(int fd, struct usb_raw_ep_io* io)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io);
}

static int usb_raw_ep0_read(int fd, struct usb_raw_ep_io* io)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_READ, io);
}

static int usb_raw_ep_enable(int fd, struct usb_endpoint_descriptor* desc)
{
  return ioctl(fd, USB_RAW_IOCTL_EP_ENABLE, desc);
}

static int usb_raw_ep_disable(int fd, int ep)
{
  return ioctl(fd, USB_RAW_IOCTL_EP_DISABLE, ep);
}

static int usb_raw_configure(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
}

static int usb_raw_vbus_draw(int fd, uint32_t power)
{
  return ioctl(fd, USB_RAW_IOCTL_VBUS_DRAW, power);
}

static int usb_raw_ep0_stall(int fd)
{
  return ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0);
}

static int lookup_interface(int fd, uint8_t bInterfaceNumber,
                            uint8_t bAlternateSetting)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  if (!index)
    return -1;
  for (int i = 0; i < index->ifaces_num; i++) {
    if (index->ifaces[i].bInterfaceNumber == bInterfaceNumber &&
        index->ifaces[i].bAlternateSetting == bAlternateSetting)
      return i;
  }
  return -1;
}

static void set_interface(int fd, int n)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  if (!index)
    return;
  if (index->iface_cur >= 0 && index->iface_cur < index->ifaces_num) {
    for (int ep = 0; ep < index->ifaces[index->iface_cur].eps_num; ep++) {
      int rv = usb_raw_ep_disable(
          fd, index->ifaces[index->iface_cur].eps[ep].handle);
      if (rv < 0) {
      } else {
      }
    }
  }
  if (n >= 0 && n < index->ifaces_num) {
    for (int ep = 0; ep < index->ifaces[n].eps_num; ep++) {
      int rv = usb_raw_ep_enable(fd, &index->ifaces[n].eps[ep].desc);
      if (rv < 0) {
      } else {
        index->ifaces[n].eps[ep].handle = rv;
      }
    }
    index->iface_cur = n;
  }
}

static int configure_device(int fd)
{
  struct usb_device_index* index = lookup_usb_index(fd);
  if (!index)
    return -1;
  int rv = usb_raw_vbus_draw(fd, index->bMaxPower);
  if (rv < 0) {
    return rv;
  }
  rv = usb_raw_configure(fd);
  if (rv < 0) {
    return rv;
  }
  set_interface(fd, 0);
  return 0;
}

#define USB_MAX_PACKET_SIZE 4096

struct usb_raw_control_event {
  struct usb_raw_event inner;
  struct usb_ctrlrequest ctrl;
  char data[USB_MAX_PACKET_SIZE];
};

struct usb_raw_ep_io_data {
  struct usb_raw_ep_io inner;
  char data[USB_MAX_PACKET_SIZE];
};

static volatile long
syz_usb_connect_impl(uint64_t speed, uint64_t dev_len, const char* dev,
                     const struct vusb_connect_descriptors* descs,
                     lookup_connect_out_response_t lookup_connect_response_out)
{
  if (!dev) {
    return -1;
  }
  int fd = usb_raw_open();
  if (fd < 0) {
    return fd;
  }
  if (fd >= MAX_FDS) {
    close(fd);
    return -1;
  }
  struct usb_device_index* index = add_usb_index(fd, dev, dev_len);
  if (!index) {
    return -1;
  }
  char device[32];
  sprintf(&device[0], "dummy_udc.%llu", procid);
  int rv = usb_raw_init(fd, speed, "dummy_udc", &device[0]);
  if (rv < 0) {
    return rv;
  }
  rv = usb_raw_run(fd);
  if (rv < 0) {
    return rv;
  }
  bool done = false;
  while (!done) {
    struct usb_raw_control_event event;
    event.inner.type = 0;
    event.inner.length = sizeof(event.ctrl);
    rv = usb_raw_event_fetch(fd, (struct usb_raw_event*)&event);
    if (rv < 0) {
      return rv;
    }
    if (event.inner.type != USB_RAW_EVENT_CONTROL)
      continue;
    char* response_data = NULL;
    uint32_t response_length = 0;
    if (event.ctrl.bRequestType & USB_DIR_IN) {
      if (!lookup_connect_response_in(fd, descs, &event.ctrl, &response_data,
                                      &response_length)) {
        usb_raw_ep0_stall(fd);
        continue;
      }
    } else {
      if (!lookup_connect_response_out(fd, descs, &event.ctrl, &done)) {
        usb_raw_ep0_stall(fd);
        continue;
      }
      response_data = NULL;
      response_length = event.ctrl.wLength;
    }
    if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD &&
        event.ctrl.bRequest == USB_REQ_SET_CONFIGURATION) {
      rv = configure_device(fd);
      if (rv < 0) {
        return rv;
      }
    }
    struct usb_raw_ep_io_data response;
    response.inner.ep = 0;
    response.inner.flags = 0;
    if (response_length > sizeof(response.data))
      response_length = 0;
    if (event.ctrl.wLength < response_length)
      response_length = event.ctrl.wLength;
    response.inner.length = response_length;
    if (response_data)
      memcpy(&response.data[0], response_data, response_length);
    else
      memset(&response.data[0], 0, response_length);
    if (event.ctrl.bRequestType & USB_DIR_IN) {
      rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io*)&response);
    } else {
      rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io*)&response);
    }
    if (rv < 0) {
      return rv;
    }
  }
  sleep_ms(200);
  return fd;
}

static volatile long syz_usb_connect(volatile long a0, volatile long a1,
                                     volatile long a2, volatile long a3)
{
  uint64_t speed = a0;
  uint64_t dev_len = a1;
  const char* dev = (const char*)a2;
  const struct vusb_connect_descriptors* descs =
      (const struct vusb_connect_descriptors*)a3;
  return syz_usb_connect_impl(speed, dev_len, dev, descs,
                              &lookup_connect_response_out_generic);
}

static volatile long syz_usb_control_io(volatile long a0, volatile long a1,
                                        volatile long a2)
{
  int fd = a0;
  const struct vusb_descriptors* descs = (const struct vusb_descriptors*)a1;
  const struct vusb_responses* resps = (const struct vusb_responses*)a2;
  struct usb_raw_control_event event;
  event.inner.type = 0;
  event.inner.length = USB_MAX_PACKET_SIZE;
  int rv = usb_raw_event_fetch(fd, (struct usb_raw_event*)&event);
  if (rv < 0) {
    return rv;
  }
  if (event.inner.type != USB_RAW_EVENT_CONTROL) {
    return -1;
  }
  char* response_data = NULL;
  uint32_t response_length = 0;
  if ((event.ctrl.bRequestType & USB_DIR_IN) && event.ctrl.wLength) {
    if (!lookup_control_response(descs, resps, &event.ctrl, &response_data,
                                 &response_length)) {
      usb_raw_ep0_stall(fd);
      return -1;
    }
  } else {
    if ((event.ctrl.bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD ||
        event.ctrl.bRequest == USB_REQ_SET_INTERFACE) {
      int iface_num = event.ctrl.wIndex;
      int alt_set = event.ctrl.wValue;
      int iface_index = lookup_interface(fd, iface_num, alt_set);
      if (iface_index < 0) {
      } else {
        set_interface(fd, iface_index);
      }
    }
    response_length = event.ctrl.wLength;
  }
  struct usb_raw_ep_io_data response;
  response.inner.ep = 0;
  response.inner.flags = 0;
  if (response_length > sizeof(response.data))
    response_length = 0;
  if (event.ctrl.wLength < response_length)
    response_length = event.ctrl.wLength;
  if ((event.ctrl.bRequestType & USB_DIR_IN) && !event.ctrl.wLength) {
    response_length = USB_MAX_PACKET_SIZE;
  }
  response.inner.length = response_length;
  if (response_data)
    memcpy(&response.data[0], response_data, response_length);
  else
    memset(&response.data[0], 0, response_length);
  if ((event.ctrl.bRequestType & USB_DIR_IN) && event.ctrl.wLength) {
    rv = usb_raw_ep0_write(fd, (struct usb_raw_ep_io*)&response);
  } else {
    rv = usb_raw_ep0_read(fd, (struct usb_raw_ep_io*)&response);
  }
  if (rv < 0) {
    return rv;
  }
  sleep_ms(200);
  return 0;
}

#define BTPROTO_HCI 1
#define ACL_LINK 1
#define SCAN_PAGE 2

typedef struct {
  uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

#define HCI_COMMAND_PKT 1
#define HCI_EVENT_PKT 4
#define HCI_VENDOR_PKT 0xff

struct hci_command_hdr {
  uint16_t opcode;
  uint8_t plen;
} __attribute__((packed));

struct hci_event_hdr {
  uint8_t evt;
  uint8_t plen;
} __attribute__((packed));

#define HCI_EV_CONN_COMPLETE 0x03
struct hci_ev_conn_complete {
  uint8_t status;
  uint16_t handle;
  bdaddr_t bdaddr;
  uint8_t link_type;
  uint8_t encr_mode;
} __attribute__((packed));

#define HCI_EV_CONN_REQUEST 0x04
struct hci_ev_conn_request {
  bdaddr_t bdaddr;
  uint8_t dev_class[3];
  uint8_t link_type;
} __attribute__((packed));

#define HCI_EV_REMOTE_FEATURES 0x0b
struct hci_ev_remote_features {
  uint8_t status;
  uint16_t handle;
  uint8_t features[8];
} __attribute__((packed));

#define HCI_EV_CMD_COMPLETE 0x0e
struct hci_ev_cmd_complete {
  uint8_t ncmd;
  uint16_t opcode;
} __attribute__((packed));

#define HCI_OP_WRITE_SCAN_ENABLE 0x0c1a

#define HCI_OP_READ_BUFFER_SIZE 0x1005
struct hci_rp_read_buffer_size {
  uint8_t status;
  uint16_t acl_mtu;
  uint8_t sco_mtu;
  uint16_t acl_max_pkt;
  uint16_t sco_max_pkt;
} __attribute__((packed));

#define HCI_OP_READ_BD_ADDR 0x1009
struct hci_rp_read_bd_addr {
  uint8_t status;
  bdaddr_t bdaddr;
} __attribute__((packed));

#define HCI_EV_LE_META 0x3e
struct hci_ev_le_meta {
  uint8_t subevent;
} __attribute__((packed));

#define HCI_EV_LE_CONN_COMPLETE 0x01
struct hci_ev_le_conn_complete {
  uint8_t status;
  uint16_t handle;
  uint8_t role;
  uint8_t bdaddr_type;
  bdaddr_t bdaddr;
  uint16_t interval;
  uint16_t latency;
  uint16_t supervision_timeout;
  uint8_t clk_accurancy;
} __attribute__((packed));

struct hci_dev_req {
  uint16_t dev_id;
  uint32_t dev_opt;
};

struct vhci_vendor_pkt {
  uint8_t type;
  uint8_t opcode;
  uint16_t id;
};

#define HCIDEVUP _IOW('H', 201, int)
#define HCISETSCAN _IOW('H', 221, int)

static int vhci_fd = -1;

static void rfkill_unblock_all()
{
  int fd = open("/dev/rfkill", O_WRONLY);
  if (fd < 0)
    exit(1);
  struct rfkill_event event = {0};
  event.idx = 0;
  event.type = RFKILL_TYPE_ALL;
  event.op = RFKILL_OP_CHANGE_ALL;
  event.soft = 0;
  event.hard = 0;
  if (write(fd, &event, sizeof(event)) < 0)
    exit(1);
  close(fd);
}

static void hci_send_event_packet(int fd, uint8_t evt, void* data,
                                  size_t data_len)
{
  struct iovec iv[3];
  struct hci_event_hdr hdr;
  hdr.evt = evt;
  hdr.plen = data_len;
  uint8_t type = HCI_EVENT_PKT;
  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = &hdr;
  iv[1].iov_len = sizeof(hdr);
  iv[2].iov_base = data;
  iv[2].iov_len = data_len;
  if (writev(fd, iv, sizeof(iv) / sizeof(struct iovec)) < 0)
    exit(1);
}

static void hci_send_event_cmd_complete(int fd, uint16_t opcode, void* data,
                                        size_t data_len)
{
  struct iovec iv[4];
  struct hci_event_hdr hdr;
  hdr.evt = HCI_EV_CMD_COMPLETE;
  hdr.plen = sizeof(struct hci_ev_cmd_complete) + data_len;
  struct hci_ev_cmd_complete evt_hdr;
  evt_hdr.ncmd = 1;
  evt_hdr.opcode = opcode;
  uint8_t type = HCI_EVENT_PKT;
  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = &hdr;
  iv[1].iov_len = sizeof(hdr);
  iv[2].iov_base = &evt_hdr;
  iv[2].iov_len = sizeof(evt_hdr);
  iv[3].iov_base = data;
  iv[3].iov_len = data_len;
  if (writev(fd, iv, sizeof(iv) / sizeof(struct iovec)) < 0)
    exit(1);
}

static bool process_command_pkt(int fd, char* buf, ssize_t buf_size)
{
  struct hci_command_hdr* hdr = (struct hci_command_hdr*)buf;
  if (buf_size < (ssize_t)sizeof(struct hci_command_hdr) ||
      hdr->plen != buf_size - sizeof(struct hci_command_hdr)) {
    exit(1);
  }
  switch (hdr->opcode) {
  case HCI_OP_WRITE_SCAN_ENABLE: {
    uint8_t status = 0;
    hci_send_event_cmd_complete(fd, hdr->opcode, &status, sizeof(status));
    return true;
  }
  case HCI_OP_READ_BD_ADDR: {
    struct hci_rp_read_bd_addr rp = {0};
    rp.status = 0;
    memset(&rp.bdaddr, 0xaa, 6);
    hci_send_event_cmd_complete(fd, hdr->opcode, &rp, sizeof(rp));
    return false;
  }
  case HCI_OP_READ_BUFFER_SIZE: {
    struct hci_rp_read_buffer_size rp = {0};
    rp.status = 0;
    rp.acl_mtu = 1021;
    rp.sco_mtu = 96;
    rp.acl_max_pkt = 4;
    rp.sco_max_pkt = 6;
    hci_send_event_cmd_complete(fd, hdr->opcode, &rp, sizeof(rp));
    return false;
  }
  }
  char dummy[0xf9] = {0};
  hci_send_event_cmd_complete(fd, hdr->opcode, dummy, sizeof(dummy));
  return false;
}

static void* event_thread(void* arg)
{
  while (1) {
    char buf[1024] = {0};
    ssize_t buf_size = read(vhci_fd, buf, sizeof(buf));
    if (buf_size < 0)
      exit(1);
    if (buf_size > 0 && buf[0] == HCI_COMMAND_PKT) {
      if (process_command_pkt(vhci_fd, buf + 1, buf_size - 1))
        break;
    }
  }
  return NULL;
}
#define HCI_HANDLE_1 200
#define HCI_HANDLE_2 201

static void initialize_vhci()
{
  int hci_sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
  if (hci_sock < 0)
    exit(1);
  vhci_fd = open("/dev/vhci", O_RDWR);
  if (vhci_fd == -1)
    exit(1);
  const int kVhciFd = 241;
  if (dup2(vhci_fd, kVhciFd) < 0)
    exit(1);
  close(vhci_fd);
  vhci_fd = kVhciFd;
  struct vhci_vendor_pkt vendor_pkt;
  if (read(vhci_fd, &vendor_pkt, sizeof(vendor_pkt)) != sizeof(vendor_pkt))
    exit(1);
  if (vendor_pkt.type != HCI_VENDOR_PKT)
    exit(1);
  pthread_t th;
  if (pthread_create(&th, NULL, event_thread, NULL))
    exit(1);
  int ret = ioctl(hci_sock, HCIDEVUP, vendor_pkt.id);
  if (ret) {
    if (errno == ERFKILL) {
      rfkill_unblock_all();
      ret = ioctl(hci_sock, HCIDEVUP, vendor_pkt.id);
    }
    if (ret && errno != EALREADY)
      exit(1);
  }
  struct hci_dev_req dr = {0};
  dr.dev_id = vendor_pkt.id;
  dr.dev_opt = SCAN_PAGE;
  if (ioctl(hci_sock, HCISETSCAN, &dr))
    exit(1);
  struct hci_ev_conn_request request;
  memset(&request, 0, sizeof(request));
  memset(&request.bdaddr, 0xaa, 6);
  *(uint8_t*)&request.bdaddr.b[5] = 0x10;
  request.link_type = ACL_LINK;
  hci_send_event_packet(vhci_fd, HCI_EV_CONN_REQUEST, &request,
                        sizeof(request));
  struct hci_ev_conn_complete complete;
  memset(&complete, 0, sizeof(complete));
  complete.status = 0;
  complete.handle = HCI_HANDLE_1;
  memset(&complete.bdaddr, 0xaa, 6);
  *(uint8_t*)&complete.bdaddr.b[5] = 0x10;
  complete.link_type = ACL_LINK;
  complete.encr_mode = 0;
  hci_send_event_packet(vhci_fd, HCI_EV_CONN_COMPLETE, &complete,
                        sizeof(complete));
  struct hci_ev_remote_features features;
  memset(&features, 0, sizeof(features));
  features.status = 0;
  features.handle = HCI_HANDLE_1;
  hci_send_event_packet(vhci_fd, HCI_EV_REMOTE_FEATURES, &features,
                        sizeof(features));
  struct {
    struct hci_ev_le_meta le_meta;
    struct hci_ev_le_conn_complete le_conn;
  } le_conn;
  memset(&le_conn, 0, sizeof(le_conn));
  le_conn.le_meta.subevent = HCI_EV_LE_CONN_COMPLETE;
  memset(&le_conn.le_conn.bdaddr, 0xaa, 6);
  *(uint8_t*)&le_conn.le_conn.bdaddr.b[5] = 0x11;
  le_conn.le_conn.role = 1;
  le_conn.le_conn.handle = HCI_HANDLE_2;
  hci_send_event_packet(vhci_fd, HCI_EV_LE_META, &le_conn, sizeof(le_conn));
  pthread_join(th, NULL);
  close(hci_sock);
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
  initialize_vhci();
  sandbox_common();
  drop_caps();
  if (unshare(CLONE_NEWNET)) {
  }
  initialize_wifi_devices();
  loop();
  exit(1);
}

static void close_fds()
{
  for (int fd = 3; fd < MAX_FDS; fd++)
    close(fd);
}

static void setup_binfmt_misc()
{
  if (mount(0, "/proc/sys/fs/binfmt_misc", "binfmt_misc", 0, 0)) {
  }
  write_file("/proc/sys/fs/binfmt_misc/register", ":syz0:M:0:\x01::./file0:");
  write_file("/proc/sys/fs/binfmt_misc/register",
             ":syz1:M:1:\x02::./file0:POC");
}

static void setup_sysctl()
{
  char mypid[32];
  snprintf(mypid, sizeof(mypid), "%d", getpid());
  struct {
    const char* name;
    const char* data;
  } files[] = {
      {"/sys/kernel/debug/x86/nmi_longest_ns", "10000000000"},
      {"/proc/sys/kernel/hung_task_check_interval_secs", "20"},
      {"/proc/sys/net/core/bpf_jit_kallsyms", "1"},
      {"/proc/sys/net/core/bpf_jit_harden", "0"},
      {"/proc/sys/kernel/kptr_restrict", "0"},
      {"/proc/sys/kernel/softlockup_all_cpu_backtrace", "1"},
      {"/proc/sys/fs/mount-max", "100"},
      {"/proc/sys/vm/oom_dump_tasks", "0"},
      {"/proc/sys/debug/exception-trace", "0"},
      {"/proc/sys/kernel/printk", "7 4 1 3"},
      {"/proc/sys/net/ipv4/ping_group_range", "0 65535"},
      {"/proc/sys/kernel/keys/gc_delay", "1"},
      {"/proc/sys/vm/oom_kill_allocating_task", "1"},
      {"/proc/sys/kernel/ctrl-alt-del", "0"},
      {"/proc/sys/kernel/cad_pid", mypid},
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (!write_file(files[i].name, files[i].data))
      printf("write to %s failed: %s\n", files[i].name, strerror(errno));
  }
}

#define NL802154_CMD_SET_SHORT_ADDR 11
#define NL802154_ATTR_IFINDEX 3
#define NL802154_ATTR_SHORT_ADDR 10

static void setup_802154()
{
  int sock_route = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (sock_route == -1)
    exit(1);
  int sock_generic = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  if (sock_generic < 0)
    exit(1);
  int nl802154_family_id =
      netlink_query_family_id(&nlmsg, sock_generic, "nl802154", true);
  for (int i = 0; i < 2; i++) {
    char devname[] = "wpan0";
    devname[strlen(devname) - 1] += i;
    uint64_t hwaddr = 0xaaaaaaaaaaaa0002 + (i << 8);
    uint16_t shortaddr = 0xaaa0 + i;
    int ifindex = if_nametoindex(devname);
    struct genlmsghdr genlhdr;
    memset(&genlhdr, 0, sizeof(genlhdr));
    genlhdr.cmd = NL802154_CMD_SET_SHORT_ADDR;
    netlink_init(&nlmsg, nl802154_family_id, 0, &genlhdr, sizeof(genlhdr));
    netlink_attr(&nlmsg, NL802154_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
    netlink_attr(&nlmsg, NL802154_ATTR_SHORT_ADDR, &shortaddr,
                 sizeof(shortaddr));
    int err = netlink_send(&nlmsg, sock_generic);
    if (err < 0) {
    }
    netlink_device_change(&nlmsg, sock_route, devname, true, 0, &hwaddr,
                          sizeof(hwaddr), 0);
    if (i == 0) {
      netlink_add_device_impl(&nlmsg, "lowpan", "lowpan0");
      netlink_done(&nlmsg);
      netlink_attr(&nlmsg, IFLA_LINK, &ifindex, sizeof(ifindex));
      int err = netlink_send(&nlmsg, sock_route);
      if (err < 0) {
      }
    }
  }
  close(sock_route);
  close(sock_generic);
}

uint64_t r[1] = {0xffffffffffffffff};

void loop(void)
{
  intptr_t res = 0;
  NONFAILING(memcpy((void*)0x20000040,
                    "\x12\x01\x00\x00\xf0\x6f\x29\x40\xcf\x12\x11\x71\x11\x44"
                    "\x00\x00\x00\x01\x09\x02\x1b\x00\x01\x00\x00\x00\x00\x09"
                    "\x04\x00\x00\x01\x03\x00\x00\x00\x09\x05\x84\x03\x00\x55",
                    42));
  res = -1;
  NONFAILING(res = syz_usb_connect(0, 0x2d, 0x20000040, 0));
  if (res != -1)
    r[0] = res;
  NONFAILING(*(uint32_t*)0x20000500 = 0x44);
  NONFAILING(*(uint64_t*)0x20000504 = 0x20000480);
  NONFAILING(*(uint8_t*)0x20000480 = 0);
  NONFAILING(*(uint8_t*)0x20000481 = 0);
  NONFAILING(*(uint32_t*)0x20000482 = 0);
  NONFAILING(*(uint64_t*)0x2000050c = 0);
  NONFAILING(*(uint64_t*)0x20000514 = 0);
  NONFAILING(*(uint64_t*)0x2000051c = 0);
  NONFAILING(*(uint64_t*)0x20000524 = 0);
  NONFAILING(*(uint64_t*)0x2000052c = 0);
  NONFAILING(*(uint64_t*)0x20000534 = 0);
  NONFAILING(*(uint64_t*)0x2000053c = 0);
  NONFAILING(syz_usb_control_io(r[0], 0, 0x20000500));
  NONFAILING(*(uint32_t*)0x20000280 = 0x84);
  NONFAILING(*(uint64_t*)0x20000284 = 0x20000b00);
  NONFAILING(*(uint8_t*)0x20000b00 = 0);
  NONFAILING(*(uint8_t*)0x20000b01 = 0);
  NONFAILING(*(uint32_t*)0x20000b02 = 0xffffff26);
  NONFAILING(*(uint64_t*)0x2000028c = 0);
  NONFAILING(*(uint64_t*)0x20000294 = 0);
  NONFAILING(*(uint64_t*)0x2000029c = 0);
  NONFAILING(*(uint64_t*)0x200002a4 = 0);
  NONFAILING(*(uint64_t*)0x200002ac = 0);
  NONFAILING(*(uint64_t*)0x200002b4 = 0);
  NONFAILING(*(uint64_t*)0x200002bc = 0);
  NONFAILING(*(uint64_t*)0x200002c4 = 0);
  NONFAILING(*(uint64_t*)0x200002cc = 0);
  NONFAILING(*(uint64_t*)0x200002d4 = 0);
  NONFAILING(*(uint64_t*)0x200002dc = 0);
  NONFAILING(*(uint64_t*)0x200002e4 = 0);
  NONFAILING(*(uint64_t*)0x200002ec = 0);
  NONFAILING(*(uint64_t*)0x200002f4 = 0);
  NONFAILING(*(uint64_t*)0x200002fc = 0);
  NONFAILING(syz_usb_control_io(r[0], 0, 0x20000280));
  close_fds();
}
int main(void)
{
  syscall(__NR_mmap, 0x1ffff000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
  syscall(__NR_mmap, 0x20000000ul, 0x1000000ul, 7ul, 0x32ul, -1, 0ul);
  syscall(__NR_mmap, 0x21000000ul, 0x1000ul, 0ul, 0x32ul, -1, 0ul);
  setup_sysctl();
  setup_binfmt_misc();
  setup_802154();
  install_segv_handler();
  do_sandbox_none();
  return 0;
}
