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

#include "config.h"

#include <functional>
#include <unistd.h>
#include <cstdio>
#include <rak/file_stat.h>
#include <rak/error_number.h>
#include <rak/path.h>
#include <rak/socket_address.h>
#include <rak/string_manip.h>
#include <torrent/rate.h>
#include <torrent/throttle.h>
#include <torrent/tracker.h>
#include <torrent/connection_manager.h>
#include <torrent/data/file.h>
#include <torrent/data/file_list.h>
#include <torrent/peer/connection_list.h>
#include <torrent/peer/peer_list.h>

#include "core/download.h"
#include "core/download_store.h"
#include "core/manager.h"
#include "rpc/command_variable.h"

#include "globals.h"
#include "control.h"
#include "command_helpers.h"

std::string
retrieve_d_base_path(core::Download* download) {
  if (download->file_list()->is_multi_file())
    return download->file_list()->frozen_root_dir();
  else
    return download->file_list()->empty() ? std::string() : download->file_list()->at(0)->frozen_path();
}

std::string
retrieve_d_base_filename(core::Download* download) {
  const std::string* base;

  if (download->file_list()->is_multi_file())
    base = &download->file_list()->frozen_root_dir();
  else
    base = &download->file_list()->at(0)->frozen_path();

  std::string::size_type split = base->rfind('/');

  if (split == std::string::npos)
    return *base;
  else
    return base->substr(split + 1);
}

torrent::Object
apply_d_change_link(int changeType, core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object::list_type& args = rawArgs.as_list();

  if (args.size() != 3)
    throw torrent::input_error("Wrong argument count.");

  torrent::Object::list_const_iterator itr = args.begin();

  const std::string& type    = (itr++)->as_string();
  const std::string& prefix  = (itr++)->as_string();
  const std::string& postfix = (itr++)->as_string();
  
  if (type.empty())
    throw torrent::input_error("Invalid arguments.");

  std::string target;
  std::string link;

  if (type == "base_path") {
    target = rpc::call_command_string("d.get_base_path", rpc::make_target(download));
    link = rak::path_expand(prefix + rpc::call_command_string("d.get_base_path", rpc::make_target(download)) + postfix);

  } else if (type == "base_filename") {
    target = rpc::call_command_string("d.get_base_path", rpc::make_target(download));
    link = rak::path_expand(prefix + rpc::call_command_string("d.get_base_filename", rpc::make_target(download)) + postfix);

//   } else if (type == "directory_path") {
//     target = rpc::call_command_string("d.get_directory", rpc::make_target(download));
//     link = rak::path_expand(prefix + rpc::call_command_string("d.get_base_path", rpc::make_target(download)) + postfix);

  } else if (type == "tied") {
    link = rak::path_expand(rpc::call_command_string("d.get_tied_to_file", rpc::make_target(download)));

    if (link.empty())
      return torrent::Object();

    link = rak::path_expand(prefix + link + postfix);
    target = rpc::call_command_string("d.get_base_path", rpc::make_target(download));

  } else {
    throw torrent::input_error("Unknown type argument.");
  }

  switch (changeType) {
  case 0:
    if (symlink(target.c_str(), link.c_str()) == -1)
      //     control->core()->push_log("create_link failed: " + std::string(rak::error_number::current().c_str()));
      //     control->core()->push_log("create_link failed: " + std::string(rak::error_number::current().c_str()) + " to " + target);
      ; // Disabled.
    break;

  case 1:
  {
    rak::file_stat fileStat;
    rak::error_number::clear_global();

    if (!fileStat.update_link(link) || !fileStat.is_link() ||
        unlink(link.c_str()) == -1)
      ; //     control->core()->push_log("delete_link failed: " + std::string(rak::error_number::current().c_str()));

    break;
  }
  default:
    break;
  }

  return torrent::Object();
}

void
apply_d_delete_tied(core::Download* download) {
  const std::string& tie = rpc::call_command_string("d.get_tied_to_file", rpc::make_target(download));

  if (tie.empty())
    return;

  if (::unlink(rak::path_expand(tie).c_str()) == -1)
    control->core()->push_log_std("Could not unlink tied file: " + std::string(rak::error_number::current().c_str()));

  rpc::call_command("d.set_tied_to_file", std::string(), rpc::make_target(download));
}

void
apply_d_connection_type(core::Download* download, const std::string& name) {
  torrent::Download::ConnectionType connType;

  if (name == "leech")
    connType = torrent::Download::CONNECTION_LEECH;
  else if (name == "seed")
    connType = torrent::Download::CONNECTION_SEED;
  else if (name == "initial_seed")
    connType = torrent::Download::CONNECTION_INITIAL_SEED;
  else
    throw torrent::input_error("Unknown peer connection type selected.");

  download->download()->set_connection_type(connType);
}

void
apply_d_directory(core::Download* download, const std::string& name) {
  if (!download->file_list()->is_multi_file())
    download->set_root_directory(name);
  else if (name.empty() || *name.rbegin() == '/')
    download->set_root_directory(name + download->download()->name());
  else
    download->set_root_directory(name + "/" + download->download()->name());
}

const char*
retrieve_d_connection_type(core::Download* download) {
  switch (download->download()->connection_type()) {
  case torrent::Download::CONNECTION_LEECH:
    return "leech";
  case torrent::Download::CONNECTION_SEED:
    return "seed";
  case torrent::Download::CONNECTION_INITIAL_SEED:
    return "initial_seed";
  default:
    return "unknown";
  }
}

const char*
retrieve_d_priority_str(core::Download* download) {
  switch (download->priority()) {
  case 0:
    return "off";
  case 1:
    return "low";
  case 2:
    return "normal";
  case 3:
    return "high";
  default:
    throw torrent::input_error("Priority out of range.");
  }
}

torrent::Object
retrieve_d_ratio(core::Download* download) {
  if (download->is_hash_checking())
    return int64_t();

  int64_t bytesDone = download->download()->bytes_done();
  int64_t upTotal   = download->download()->up_rate()->total();

  return bytesDone > 0 ? (1000 * upTotal) / bytesDone : 0;
}

torrent::Object
retrieve_d_hash(core::Download* download) {
  const torrent::HashString* hashString = &download->download()->info_hash();

  return torrent::Object(rak::transform_hex(hashString->begin(), hashString->end()));
}

torrent::Object
retrieve_d_local_id(core::Download* download) {
  const torrent::HashString* hashString = &download->download()->local_id();

  return torrent::Object(rak::transform_hex(hashString->begin(), hashString->end()));
}

torrent::Object
retrieve_d_local_id_html(core::Download* download) {
  const torrent::HashString* hashString = &download->download()->local_id();

  return torrent::Object(rak::copy_escape_html(hashString->begin(), hashString->end()));
}

torrent::Object
apply_d_custom(core::Download* download, const torrent::Object& rawArgs) {
  torrent::Object::list_const_iterator itr = rawArgs.as_list().begin();
  if (itr == rawArgs.as_list().end())
    throw torrent::bencode_error("Missing key argument.");

  const std::string& key = itr->as_string();
  if (++itr == rawArgs.as_list().end())
    throw torrent::bencode_error("Missing value argument.");

  download->bencode()->get_key("rtorrent").
                       insert_preserve_copy("custom", torrent::Object::create_map()).first->second.
                       insert_key(key, itr->as_string());
  return torrent::Object();
}

torrent::Object
retrieve_d_custom(core::Download* download, const std::string& key) {
  try {
    return download->bencode()->get_key("rtorrent").get_key("custom").get_key_string(key);

  } catch (torrent::bencode_error& e) {
    return std::string();
  }
}

torrent::Object
retrieve_d_custom_throw(core::Download* download, const std::string& key) {
  try {
    return download->bencode()->get_key("rtorrent").get_key("custom").get_key_string(key);

  } catch (torrent::bencode_error& e) {
    throw torrent::input_error("No such custom value.");
  }
}

torrent::Object
retrieve_d_bitfield(core::Download* download) {
  const torrent::Bitfield* bitField = download->download()->file_list()->bitfield();

  if (bitField->empty())
    return torrent::Object("");

  return torrent::Object(rak::transform_hex(bitField->begin(), bitField->end()));
}

// Just a helper function atm.
torrent::Object
cmd_d_initialize_logs(core::Download* download) {
  download->download()->signal_network_log(sigc::mem_fun(control->core(), &core::Manager::push_log_complete));
  download->download()->signal_storage_error(sigc::mem_fun(control->core(), &core::Manager::push_log_complete));

  if (!rpc::call_command_string("get_log.tracker").empty())
    download->download()->signal_tracker_dump(sigc::ptr_fun(&core::receive_tracker_dump));

  return torrent::Object();
}

struct call_add_d_peer_t {
  call_add_d_peer_t(core::Download* d, int port) : m_download(d), m_port(port) { }

  void operator() (const sockaddr* sa, int err) {
    if (sa == NULL)
      control->core()->push_log("Could not resolve host.");
    else
      m_download->download()->add_peer(sa, m_port);
  }

  core::Download* m_download;
  int m_port;
};

void
apply_d_add_peer(core::Download* download, const std::string& arg) {
  int port, ret;
  char dummy;
  char host[1024];

  if (download->download()->is_private())
    throw torrent::input_error("Download is private.");

  ret = std::sscanf(arg.c_str(), "%1023[^:]:%i%c", host, &port, &dummy);

  if (ret == 1)
    port = 6881;
  else if (ret != 2)
    throw torrent::input_error("Could not parse host.");

  if (port < 1 || port > 65535)
    throw torrent::input_error("Invalid port number.");

  torrent::connection_manager()->resolver()(host, (int)rak::socket_address::pf_inet, SOCK_STREAM, call_add_d_peer_t(download, port));
}

torrent::Object
f_multicall(core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object::list_type& args = rawArgs.as_list();

  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  torrent::Object             resultRaw = torrent::Object::create_list();
  torrent::Object::list_type& result = resultRaw.as_list();

  for (torrent::FileList::const_iterator itr = download->file_list()->begin(), last = download->file_list()->end(); itr != last; itr++) {
    torrent::Object::list_type& row = result.insert(result.end(), torrent::Object::create_list())->as_list();

    for (torrent::Object::list_const_iterator cItr = ++args.begin(), cLast = args.end(); cItr != args.end(); cItr++) {
      const std::string& cmd = cItr->as_string();
      row.push_back(rpc::parse_command(rpc::make_target(*itr), cmd.c_str(), cmd.c_str() + cmd.size()).first);
    }
  }

  return resultRaw;
}

torrent::Object
t_multicall(core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object::list_type& args = rawArgs.as_list();

  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  torrent::Object             resultRaw = torrent::Object::create_list();
  torrent::Object::list_type& result = resultRaw.as_list();

  for (int itr = 0, last = download->tracker_list()->size(); itr != last; itr++) {
    torrent::Object::list_type& row = result.insert(result.end(), torrent::Object::create_list())->as_list();

    for (torrent::Object::list_const_iterator cItr = ++args.begin(), cLast = args.end(); cItr != args.end(); cItr++) {
      const std::string& cmd = cItr->as_string();
      torrent::Tracker* t = download->tracker_list()->at(itr);

      row.push_back(rpc::parse_command(rpc::make_target(t), cmd.c_str(), cmd.c_str() + cmd.size()).first);
    }
  }

  return resultRaw;
}

torrent::Object
p_multicall(core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object::list_type& args = rawArgs.as_list();

  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  torrent::Object             resultRaw = torrent::Object::create_list();
  torrent::Object::list_type& result = resultRaw.as_list();

  for (torrent::ConnectionList::const_iterator itr = download->connection_list()->begin(), last = download->connection_list()->end(); itr != last; itr++) {
    torrent::Object::list_type& row = result.insert(result.end(), torrent::Object::create_list())->as_list();

    for (torrent::Object::list_const_iterator cItr = ++args.begin(), cLast = args.end(); cItr != args.end(); cItr++) {
      const std::string& cmd = cItr->as_string();

      row.push_back(rpc::parse_command(rpc::make_target(*itr), cmd.c_str(), cmd.c_str() + cmd.size()).first);
    }
  }

  return resultRaw;
}

inline torrent::Object&
d_object_wrapper(const std::pair<const char*, const char*> keyPair, core::Download* download) {
  if (keyPair.first == NULL)
    return download->bencode()->get_key(keyPair.second);
  else
    return download->bencode()->get_key(keyPair.first).get_key(keyPair.second);
}

torrent::Object
d_object_get(const std::pair<const char*, const char*> keyPair, core::Download* download, __UNUSED const torrent::Object& rawArgs) {
  return d_object_wrapper(keyPair, download);
}

torrent::Object
d_list_push_back(const std::pair<const char*, const char*> keyPair, core::Download* download, const torrent::Object& rawArgs) {
  d_object_wrapper(keyPair, download).as_list().push_back(rawArgs);

  return torrent::Object();
}

torrent::Object
d_list_push_back_unique(const std::pair<const char*, const char*> keyPair, core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object& args = (rawArgs.is_list() && !rawArgs.as_list().empty()) ? rawArgs.as_list().front() : rawArgs;
  torrent::Object::list_type& list = d_object_wrapper(keyPair, download).as_list();

  if (std::find_if(list.begin(), list.end(),
                   rak::bind1st(std::ptr_fun(&torrent::object_equal), args)) == list.end())
    list.push_back(rawArgs);

  return torrent::Object();
}

torrent::Object
d_list_has(const std::pair<const char*, const char*> keyPair, core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object& args = (rawArgs.is_list() && !rawArgs.as_list().empty()) ? rawArgs.as_list().front() : rawArgs;
  torrent::Object::list_type& list = d_object_wrapper(keyPair, download).as_list();

  return (int64_t)(std::find_if(list.begin(), list.end(),
                                rak::bind1st(std::ptr_fun(&torrent::object_equal), args)) != list.end());
}

torrent::Object
d_list_remove(const std::pair<const char*, const char*> keyPair, core::Download* download, const torrent::Object& rawArgs) {
  const torrent::Object& args = (rawArgs.is_list() && !rawArgs.as_list().empty()) ? rawArgs.as_list().front() : rawArgs;
  torrent::Object::list_type& list = d_object_wrapper(keyPair, download).as_list();

  list.erase(std::remove_if(list.begin(), list.end(), rak::bind1st(std::ptr_fun(&torrent::object_equal), args)), list.end());

  return torrent::Object();
}

#define ADD_CD_SLOT(key, function, slot, parm, doc)    \
  commandDownloadSlotsItr->set_slot(slot); \
  rpc::commands.insert_type(key, commandDownloadSlotsItr++, &rpc::CommandSlot<core::Download*>::function, rpc::CommandMap::flag_dont_delete, parm, doc);

#define ADD_CD_SLOT_PUBLIC(key, function, slot, parm, doc)    \
  commandDownloadSlotsItr->set_slot(slot); \
  rpc::commands.insert_type(key, commandDownloadSlotsItr++, &rpc::CommandSlot<core::Download*>::function, rpc::CommandMap::flag_dont_delete | rpc::CommandMap::flag_public_xmlrpc, parm, doc);

#define ADD_CD_VOID(key, slot) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_fn(slot), "i:", "")

#define ADD_CD_V_VOID(key, slot) \
  ADD_CD_SLOT_PUBLIC("d." key, call_unknown, rpc::object_fn(slot), "i:", "")

#define ADD_CD_F_VOID(key, slot) \
  ADD_CD_SLOT_PUBLIC("d." key, call_unknown, rpc::object_void_fn<core::Download*>(slot), "i:", "")

#define ADD_CD_LIST_OBSOLETE(key, slot) \
  ADD_CD_SLOT_PUBLIC(key, call_list, slot, "i:", "")

#define ADD_CD_LIST(key, slot) \
  ADD_CD_SLOT_PUBLIC("d." key, call_list, slot, "i:", "")

#define ADD_CD_STRING(key, slot) \
  ADD_CD_SLOT_PUBLIC("d." key, call_string, rpc::object_string_fn<core::Download*>(slot), "i:s", "")

#define ADD_CD_VARIABLE_VALUE(key, firstKey, secondKey) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::get_variable_d_fn(firstKey, secondKey), "i:", ""); \
  ADD_CD_SLOT       ("d.set_" key, call_value,   rpc::set_variable_d_fn(firstKey, secondKey), "i:i", "");

#define ADD_CD_VARIABLE_VALUE_PUBLIC(key, firstKey, secondKey) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::get_variable_d_fn(firstKey, secondKey), "i:", ""); \
  ADD_CD_SLOT_PUBLIC("d.set_" key, call_value,   rpc::set_variable_d_fn(firstKey, secondKey), "i:i", "");

#define ADD_CD_VARIABLE_STRING(key, firstKey, secondKey) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::get_variable_d_fn(firstKey, secondKey), "i:", ""); \
  ADD_CD_SLOT       ("d.set_" key, call_string,  rpc::set_variable_d_fn(firstKey, secondKey), "i:s", "");

#define ADD_CD_VARIABLE_STRING_PUBLIC(key, firstKey, secondKey) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::get_variable_d_fn(firstKey, secondKey), "i:", ""); \
  ADD_CD_SLOT_PUBLIC("d.set_" key, call_string,  rpc::set_variable_d_fn(firstKey, secondKey), "i:s", "");

#define ADD_CD_VALUE(key, get) \
  ADD_CD_SLOT_PUBLIC("d." key, call_unknown, rpc::object_void_fn<core::Download*>(get), "i:", "")

#define ADD_CD_VALUE_UNI(key, get) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_void_fn<core::Download*>(get), "i:", "")

#define ADD_CD_VALUE_BI(key, set, get) \
  ADD_CD_SLOT_PUBLIC("d.set_" key, call_value, rpc::object_value_fn<core::Download*>(set), "i:i", "") \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_void_fn<core::Download*>(get), "i:", "")

#define ADD_CD_VALUE_MEM_BI(key, target, set, get) \
  ADD_CD_VALUE_BI(key, rak::on2(std::mem_fun(target), std::mem_fun(set)), rak::on(std::mem_fun(target), std::mem_fun(get)));

#define ADD_CD_VALUE_MEM_UNI(key, target, get) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_void_fn<core::Download*>(rak::on(rak::on(std::mem_fun(&core::Download::download), std::mem_fun(target)), std::mem_fun(get))), "i:", "");

#define ADD_CD_STRING_UNI(key, get) \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_void_fn<core::Download*>(get), "s:", "")

#define ADD_CD_STRING_BI(key, set, get) \
  ADD_CD_SLOT_PUBLIC("d.set_" key, call_string,  rpc::object_string_fn<core::Download*>(set), "i:s", "") \
  ADD_CD_SLOT_PUBLIC("d.get_" key, call_unknown, rpc::object_void_fn<core::Download*>(get), "s:", "")

void
add_copy_to_download(const char* src, const char* dest) {
  rpc::CommandMap::iterator itr = rpc::commands.find(src);

  if (itr == rpc::commands.end())
    throw torrent::internal_error("add_copy_to_download(...) key not found.");

  rpc::commands.insert(dest, itr->second);
}

void
initialize_command_download() {
  ADD_CD_VOID("hash",          &retrieve_d_hash);
  ADD_CD_VOID("local_id",      &retrieve_d_local_id);
  ADD_CD_VOID("local_id_html", &retrieve_d_local_id_html);
  ADD_CD_VOID("bitfield",      &retrieve_d_bitfield);
  ADD_CD_VOID("base_path",     &retrieve_d_base_path);
  ADD_CD_VOID("base_filename", &retrieve_d_base_filename);
  ADD_CD_STRING_UNI("name",    rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::name)));

  // ?????
  ADD_CD_LIST_OBSOLETE("create_link",   rak::bind_ptr_fn(&apply_d_change_link, 0));
  ADD_CD_LIST_OBSOLETE("delete_link",   rak::bind_ptr_fn(&apply_d_change_link, 1));

  ADD_CD_LIST("create_link",   rak::bind_ptr_fn(&apply_d_change_link, 0));
  ADD_CD_LIST("delete_link",   rak::bind_ptr_fn(&apply_d_change_link, 1));
  ADD_CD_V_VOID("delete_tied", &apply_d_delete_tied);

  CMD_FUNC_SINGLE("d.start",     "d.set_hashing_failed=0 ;view.set_visible=started");
  CMD_FUNC_SINGLE("d.stop",      "view.set_visible=stopped");
  CMD_FUNC_SINGLE("d.try_start", "branch=\"or={d.get_hashing_failed=,d.get_ignore_commands=}\",{},{view.set_visible=started}");
  CMD_FUNC_SINGLE("d.try_stop",  "branch=d.get_ignore_commands=, {}, {view.set_visible=stopped}");
  CMD_FUNC_SINGLE("d.try_close", "branch=d.get_ignore_commands=, {}, {view.set_visible=stopped, d.close=}");

  ADD_CD_F_VOID("resume",     rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::resume_default));
  ADD_CD_F_VOID("pause",      rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::pause_default));
  ADD_CD_F_VOID("open",       rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::open_throw));
  ADD_CD_F_VOID("close",      rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::close_throw));
  ADD_CD_F_VOID("erase",      rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::erase_ptr));
  ADD_CD_F_VOID("check_hash", rak::make_mem_fun(control->core()->download_list(), &core::DownloadList::check_hash));

  ADD_CD_F_VOID("save_session",     rak::make_mem_fun(control->core()->download_store(), &core::DownloadStore::save));

  ADD_CD_F_VOID("update_priorities", rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::update_priorities)));

  ADD_CD_STRING("add_peer",        std::ptr_fun(&apply_d_add_peer));

  ADD_CD_VALUE("is_open",          rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_open)));
  ADD_CD_VALUE("is_active",        rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_active)));
  ADD_CD_VALUE("is_hash_checked",  rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_hash_checked)));
  ADD_CD_VALUE("is_hash_checking", rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_hash_checking)));
  ADD_CD_VALUE("is_multi_file",    rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::is_multi_file)));
  ADD_CD_VALUE("is_private",       rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_private)));
  ADD_CD_VALUE("is_pex_active",    rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::is_pex_active)));

  ADD_CD_VARIABLE_STRING_PUBLIC("custom1", "rtorrent", "custom1");
  ADD_CD_VARIABLE_STRING_PUBLIC("custom2", "rtorrent", "custom2");
  ADD_CD_VARIABLE_STRING_PUBLIC("custom3", "rtorrent", "custom3");
  ADD_CD_VARIABLE_STRING_PUBLIC("custom4", "rtorrent", "custom4");
  ADD_CD_VARIABLE_STRING_PUBLIC("custom5", "rtorrent", "custom5");

  ADD_CD_SLOT_PUBLIC("d.set_custom",       call_list,   rak::ptr_fn(&apply_d_custom), "i:", "");
  ADD_CD_SLOT_PUBLIC("d.get_custom",       call_string, rpc::object_string_fn<core::Download*>(std::ptr_fun(&retrieve_d_custom)), "s:s", "");
  ADD_CD_SLOT_PUBLIC("d.get_custom_throw", call_string, rpc::object_string_fn<core::Download*>(std::ptr_fun(&retrieve_d_custom_throw)), "s:s", "");

  // 0 - stopped
  // 1 - started
  ADD_CD_VARIABLE_VALUE("state", "rtorrent", "state");
  ADD_CD_VARIABLE_VALUE("complete", "rtorrent", "complete");

  // 0 off
  // 1 scheduled, being controlled by a download scheduler. Includes a priority.
  // 3 forced off
  // 2 forced on
  ADD_CD_VARIABLE_VALUE("mode", "rtorrent", "mode");

  // 0 - Not hashing
  // 1 - Normal hashing
  // 2 - Download finished, hashing
  // 3 - Rehashing
  ADD_CD_VARIABLE_VALUE("hashing",       "rtorrent", "hashing");

  // 'tied_to_file' is the file the download is associated with, and
  // can be changed by the user.
  //
  // 'loaded_file' is the file this instance of the torrent was loaded
  // from, and should not be changed.
  ADD_CD_VARIABLE_STRING_PUBLIC("tied_to_file", "rtorrent", "tied_to_file");
  ADD_CD_VARIABLE_STRING       ("loaded_file",  "rtorrent", "loaded_file");

  // The "state_changed" variable is required to be a valid unix time
  // value, it indicates the last time the torrent changed its state,
  // resume/pause.
  ADD_CD_VARIABLE_VALUE("state_changed",          "rtorrent", "state_changed");
  ADD_CD_VARIABLE_VALUE("state_counter",          "rtorrent", "state_counter");
  ADD_CD_VARIABLE_VALUE_PUBLIC("ignore_commands", "rtorrent", "ignore_commands");

  ADD_CD_STRING_BI("connection_current", std::ptr_fun(&apply_d_connection_type), std::ptr_fun(&retrieve_d_connection_type));
  ADD_CD_VARIABLE_STRING("connection_leech",      "rtorrent", "connection_leech");
  ADD_CD_VARIABLE_STRING("connection_seed",       "rtorrent", "connection_seed");

  ADD_CD_VALUE_BI("hashing_failed",      std::mem_fun(&core::Download::set_hash_failed), std::mem_fun(&core::Download::is_hash_failed));

  CMD_D("d.views",                  rak::bind_ptr_fn(&d_object_get, std::make_pair("rtorrent", "views")));
  CMD_D("d.views.has",              rak::bind_ptr_fn(&d_list_has, std::make_pair("rtorrent", "views")));
  CMD_D("d.views.remove",           rak::bind_ptr_fn(&d_list_remove, std::make_pair("rtorrent", "views")));
  CMD_D("d.views.push_back",        rak::bind_ptr_fn(&d_list_push_back, std::make_pair("rtorrent", "views")));
  CMD_D("d.views.push_back_unique", rak::bind_ptr_fn(&d_list_push_back_unique, std::make_pair("rtorrent", "views")));

  // This command really needs to be improved, so we have proper
  // logging support.
  ADD_CD_STRING_BI("message",            std::mem_fun(&core::Download::set_message), std::mem_fun(&core::Download::message));

  ADD_CD_VALUE_MEM_BI("max_file_size", &core::Download::file_list, &torrent::FileList::set_max_file_size, &torrent::FileList::max_file_size);

  ADD_CD_VALUE_MEM_BI("peers_min",        &core::Download::connection_list, &torrent::ConnectionList::set_min_size, &torrent::ConnectionList::min_size);
  ADD_CD_VALUE_MEM_BI("peers_max",        &core::Download::connection_list, &torrent::ConnectionList::set_max_size, &torrent::ConnectionList::max_size);
  ADD_CD_VALUE_MEM_BI("uploads_max",      &core::Download::download, &torrent::Download::set_uploads_max, &torrent::Download::uploads_max);
  ADD_CD_VALUE_UNI("peers_connected",     std::mem_fun(&core::Download::connection_list_size));
  ADD_CD_VALUE_UNI("peers_not_connected", rak::on(std::mem_fun(&core::Download::c_peer_list), std::mem_fun(&torrent::PeerList::available_list_size)));
  ADD_CD_VALUE_UNI("peers_complete",      rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::peers_complete)));
  ADD_CD_VALUE_UNI("peers_accounted",     rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::peers_accounted)));

  ADD_CD_VALUE_MEM_BI("peer_exchange", &core::Download::download, &torrent::Download::set_pex_enabled, &torrent::Download::is_pex_enabled);

  ADD_CD_VALUE_MEM_UNI("up_rate",      &torrent::Download::mutable_up_rate, &torrent::Rate::rate);
  ADD_CD_VALUE_MEM_UNI("up_total",     &torrent::Download::mutable_up_rate, &torrent::Rate::total);
  ADD_CD_VALUE_MEM_UNI("down_rate",    &torrent::Download::mutable_down_rate, &torrent::Rate::rate);
  ADD_CD_VALUE_MEM_UNI("down_total",   &torrent::Download::mutable_down_rate, &torrent::Rate::total);
  ADD_CD_VALUE_MEM_UNI("skip_rate",    &torrent::Download::mutable_skip_rate, &torrent::Rate::rate);
  ADD_CD_VALUE_MEM_UNI("skip_total",   &torrent::Download::mutable_skip_rate, &torrent::Rate::total);

  ADD_CD_STRING("set_throttle_name",      std::mem_fun(&core::Download::set_throttle_name));
  ADD_CD_SLOT_PUBLIC("d.get_throttle_name", call_unknown, rpc::get_variable_d_fn("rtorrent", "throttle_name"), "i:", "");

  ADD_CD_VALUE_UNI("creation_date",       rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::creation_date)));
  ADD_CD_VALUE_UNI("bytes_done",          rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::bytes_done)));
  ADD_CD_VALUE_UNI("ratio",               std::ptr_fun(&retrieve_d_ratio));
  ADD_CD_VALUE_UNI("chunks_hashed",       rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::chunks_hashed)));
  ADD_CD_VALUE_UNI("free_diskspace",      rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::free_diskspace)));

  ADD_CD_VALUE_UNI("size_files",          rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::size_files)));
  ADD_CD_VALUE_UNI("size_bytes",          rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::size_bytes)));
  ADD_CD_VALUE_UNI("size_chunks",         rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::size_chunks)));
  ADD_CD_VALUE_UNI("size_pex",            rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::size_pex)));
  ADD_CD_VALUE_UNI("max_size_pex",        rak::on(std::mem_fun(&core::Download::download), std::mem_fun(&torrent::Download::max_size_pex)));

  ADD_CD_VALUE_UNI("completed_bytes",     rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::completed_bytes)));
  ADD_CD_VALUE_UNI("completed_chunks",    rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::completed_chunks)));
  ADD_CD_VALUE_UNI("left_bytes",          rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::left_bytes)));

  ADD_CD_VALUE_UNI("chunk_size",          rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::chunk_size)));

  ADD_CD_VALUE_MEM_BI("tracker_numwant",  &core::Download::tracker_list, &torrent::TrackerList::set_numwant, &torrent::TrackerList::numwant);
  ADD_CD_VALUE_UNI("tracker_focus",       rak::on(std::mem_fun(&core::Download::tracker_list), std::mem_fun(&torrent::TrackerList::focus_index)));
  ADD_CD_VALUE_UNI("tracker_size",        std::mem_fun(&core::Download::tracker_list_size));

  ADD_CD_STRING_BI("directory",           std::ptr_fun(&apply_d_directory), rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::root_dir)));
  ADD_CD_STRING_BI("directory_base",      std::mem_fun(&core::Download::set_root_directory), rak::on(std::mem_fun(&core::Download::file_list), std::mem_fun(&torrent::FileList::root_dir)));

  ADD_CD_VALUE_BI("priority",             std::mem_fun(&core::Download::set_priority), std::mem_fun(&core::Download::priority));
  ADD_CD_STRING_UNI("priority_str",       std::ptr_fun(&retrieve_d_priority_str));

  ADD_CD_SLOT_PUBLIC("f.multicall",       call_list, rak::ptr_fn(&f_multicall), "i:", "");
  ADD_CD_SLOT_PUBLIC("p.multicall",       call_list, rak::ptr_fn(&p_multicall), "i:", "");
  ADD_CD_SLOT_PUBLIC("t.multicall",       call_list, rak::ptr_fn(&t_multicall), "i:", "");

  // NEWISH:
  CMD_D_VOID("d.initialize_logs",         &cmd_d_initialize_logs);
}
