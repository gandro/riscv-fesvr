#include "device.h"
#include "term.h"
#include "htif.h"
#include <cassert>
#include <algorithm>
#include <climits>
#include <iostream>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
using namespace std::placeholders;

device_t::device_t()
  : command_handlers(command_t::MAX_COMMANDS),
    command_names(command_t::MAX_COMMANDS)
{
  for (size_t cmd = 0; cmd < command_t::MAX_COMMANDS; cmd++)
    register_command(cmd, std::bind(&device_t::handle_null_command, this, _1), "");
  register_command(command_t::MAX_COMMANDS-1, std::bind(&device_t::handle_identify, this, _1), "identity");
}

void device_t::register_command(size_t cmd, command_func_t handler, const char* name)
{
  assert(cmd < command_t::MAX_COMMANDS);
  assert(strlen(name) < IDENTITY_SIZE);
  command_handlers[cmd] = handler;
  command_names[cmd] = name;
}

void device_t::handle_command(command_t cmd)
{
  command_handlers[cmd.cmd()](cmd);
}

void device_t::handle_null_command(command_t cmd)
{
}

void device_t::handle_identify(command_t cmd)
{
  size_t what = cmd.payload() % command_t::MAX_COMMANDS;
  uint64_t addr = cmd.payload() / command_t::MAX_COMMANDS;
  assert(addr % IDENTITY_SIZE == 0);

  char id[IDENTITY_SIZE] = {0};
  if (what == command_t::MAX_COMMANDS-1)
  {
    assert(strlen(identity()) < IDENTITY_SIZE);
    strcpy(id, identity());
  }
  else
    strcpy(id, command_names[what].c_str());

  cmd.htif()->memif().write(addr, IDENTITY_SIZE, id);
  cmd.respond(1);
}

bcd_t::bcd_t()
{
  register_command(0, std::bind(&bcd_t::handle_read, this, _1), "read");
  register_command(1, std::bind(&bcd_t::handle_write, this, _1), "write");
}

void bcd_t::handle_read(command_t cmd)
{
  pending_reads.push(cmd);
}

void bcd_t::handle_write(command_t cmd)
{
  canonical_terminal_t::write(cmd.payload());
  cmd.respond(0x100 | (uint8_t)cmd.payload());
}

void bcd_t::tick()
{
  int ch;
  if (!pending_reads.empty() && (ch = canonical_terminal_t::read()) != -1)
  {
    pending_reads.front().respond(0x100 | ch);
    pending_reads.pop();
  }
}

disk_t::disk_t(const char* fn)
{
  fd = ::open(fn, O_RDWR);
  if (fd < 0)
    throw std::runtime_error("could not open " + std::string(fn));

  register_command(0, std::bind(&disk_t::handle_read, this, _1), "read");
  register_command(1, std::bind(&disk_t::handle_write, this, _1), "write");

  struct stat st;
  if (fstat(fd, &st) < 0)
    throw std::runtime_error("could not stat " + std::string(fn));

  size = st.st_size;
  id = "disk size=" + std::to_string(size);
}

disk_t::~disk_t()
{
  close(fd);
}

void disk_t::handle_read(command_t cmd)
{
  request_t req;
  cmd.htif()->memif().read(cmd.payload(), sizeof(req), &req);

  std::vector<uint8_t> buf(req.size);
  if ((size_t)::pread(fd, &buf[0], buf.size(), req.offset) != req.size)
    throw std::runtime_error("could not read " + id + " @ " + std::to_string(req.offset));

  cmd.htif()->memif().write(req.addr, buf.size(), &buf[0]);
  cmd.respond(req.tag);
}

void disk_t::handle_write(command_t cmd)
{
  request_t req;
  cmd.htif()->memif().read(cmd.payload(), sizeof(req), &req);

  std::vector<uint8_t> buf(req.size);
  cmd.htif()->memif().read(req.addr, buf.size(), &buf[0]);

  if ((size_t)::pwrite(fd, &buf[0], buf.size(), req.offset) != req.size)
    throw std::runtime_error("could not write " + id + " @ " + std::to_string(req.offset));

  cmd.respond(req.tag);
}


char_t::char_t(const char *fn)
{
  sockaddr_un local;

  fd = -1;
  servfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (servfd < 0)
    throw std::system_error(errno, std::system_category(), "failed to create socket'");

  local.sun_family = AF_UNIX;
  ::strcpy(local.sun_path, fn);
  ::unlink(local.sun_path);

  size_t slen = ::strlen(local.sun_path) + sizeof(local.sun_family);
  if (::bind(servfd, (struct sockaddr *)&local, slen) == -1)
    throw std::system_error(errno, std::system_category(), "bind failed()");

  if (::listen(servfd, 0) == -1)
    throw std::system_error(errno, std::system_category(), "listen() failed");

  int flags = ::fcntl(servfd, F_GETFL, 0);
  if (::fcntl(servfd, F_SETFL, flags | O_NONBLOCK) == -1)
    throw std::system_error(errno, std::system_category(),
                            "failed to configure non-blocking socket");

  register_command(0, std::bind(&char_t::handle_read, this, _1), "read");
  register_command(1, std::bind(&char_t::handle_write, this, _1), "write");
  register_command(2, std::bind(&char_t::handle_poll, this, _1), "poll");

  id = std::string("char unix=") + std::string(fn);
}

void char_t::handle_read(command_t cmd)
{
  request_t req;
  cmd.htif()->memif().read(cmd.payload(), sizeof(req), &req);

  if (fd < 0) {
    req.size = 0;
    cmd.htif()->memif().write(cmd.payload(), sizeof(req), &req);
    cmd.respond(req.tag);
    return;
  }

  std::vector<uint8_t> buf(req.size);
  ssize_t n = ::read(fd, &buf[0], buf.size());

  switch (n) {
  case -1:
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::system_error(errno, std::system_category(), "read() failed");
    n = 0;
    break;
  case 0:
    close(fd);
    fd = -1;
    break;
  default:
    cmd.htif()->memif().write(req.addr, n, &buf[0]);
  }

  req.size = n;
  cmd.htif()->memif().write(cmd.payload(), sizeof(req), &req);

  cmd.respond(req.tag);
}

void char_t::handle_write(command_t cmd)
{
  request_t req;
  cmd.htif()->memif().read(cmd.payload(), sizeof(req), &req);

  if (fd < 0) {
    cmd.respond(req.tag);
    return;
  }

  std::vector<uint8_t> buf(req.size);
  cmd.htif()->memif().read(req.addr, buf.size(), &buf[0]);
  ssize_t n = ::write(fd, &buf[0], buf.size());

  switch (n) {
  case -1:
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::system_error(errno, std::system_category(), "write() failed");
    n = 0;
    break;
  case 0:
    close(fd);
    fd = -1;
    break;
  default:
    req.size -= n;
    req.addr += n;
    cmd.htif()->memif().write(cmd.payload(), sizeof(req), &req);
  }

  cmd.respond(req.tag);
}

void char_t::handle_poll(command_t cmd)
{
  pollfd pfd;
  uint16_t events = cmd.payload();
  uint16_t revents = 0;

  pfd.fd = fd;

  if (events & char_t::CHAR_POLLIN)
    pfd.events |= POLLIN;
  if (events & char_t::CHAR_POLLOUT)
    pfd.events |= POLLOUT;

  int rv = ::poll(&pfd, 1, 0);
  switch (rv) {
  case -1:
    throw std::system_error(errno, std::system_category(), "poll() failed");
  case 0:
    revents |= char_t::CHAR_POLLHUP;
    break;
  case 1:
    if (pfd.revents & POLLIN)
      revents |= char_t::CHAR_POLLIN;
    if (pfd.revents & POLLOUT)
      revents |= char_t::CHAR_POLLOUT;
    if (pfd.revents & POLLHUP)
      revents |= char_t::CHAR_POLLHUP;
    break;
  default:
    throw std::runtime_error("poll() returned unexpected value");
  }

  cmd.respond(revents);
}

void char_t::tick()
{
  if (fd == -1) {
    sockaddr_un remote;
    socklen_t socklen = sizeof(remote);
    fd = ::accept(servfd, (struct sockaddr *)&remote, &socklen);
    if (fd == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        throw std::system_error(errno, std::system_category(), "accept() failed");
    }
  }
}

device_list_t::device_list_t()
  : devices(command_t::MAX_COMMANDS, &null_device), num_devices(0)
{
}

void device_list_t::register_device(device_t* dev)
{
  num_devices++;
  assert(num_devices < command_t::MAX_DEVICES);
  devices[num_devices-1] = dev;
}

void device_list_t::handle_command(command_t cmd)
{
  devices[cmd.device()]->handle_command(cmd);
}

void device_list_t::tick()
{
  for (size_t i = 0; i < num_devices; i++)
    devices[i]->tick();
}
