// rTorrent - BitTorrent client
// Copyright (C) 2005-2007, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#ifndef RTORRENT_CORE_POLL_MANAGER_H
#define RTORRENT_CORE_POLL_MANAGER_H

#include <rak/timer.h>
#include <sigc++/signal.h>
#include <torrent/poll.h>

#include "curl_stack.h"

namespace core {

// CurlStack really should be somewhere else, but that won't happen
// until they add an epoll friendly API.

class PollManager {
public:
  typedef sigc::signal0<void> Signal;

  PollManager(torrent::Poll* poll);
  virtual ~PollManager();

  unsigned int        get_open_max() const         { return m_poll->open_max(); }

  torrent::Poll*      get_torrent_poll()           { return m_poll; }

  virtual void        poll(rak::timer timeout) = 0;
  virtual void        poll_simple(rak::timer timeout) = 0;

  static PollManager* create_poll_manager();

protected:
  PollManager(const PollManager&);
  void operator = (const PollManager&);

  void                check_error();

  torrent::Poll*      m_poll;
};

}

#endif
