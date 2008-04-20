/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "DHTEntryPointNameResolveCommand.h"
#include "DownloadEngine.h"
#include "NameResolver.h"
#include "DNSCache.h"
#include "DlAbortEx.h"
#include "prefs.h"
#include "message.h"
#include "Util.h"
#include "Option.h"
#include "DHTNode.h"
#include "DHTTaskQueue.h"
#include "DHTTaskFactory.h"
#include "DHTRoutingTable.h"
#include "DHTTask.h"
#include "RequestGroupMan.h"
#include "Logger.h"

namespace aria2 {

DHTEntryPointNameResolveCommand::DHTEntryPointNameResolveCommand(int32_t cuid, DownloadEngine* e, const std::deque<std::pair<std::string, uint16_t> >& entryPoints):
  Command(cuid),
  _e(e),
  _resolver(new NameResolver()),
  _entryPoints(entryPoints),
  _bootstrapEnabled(false)
{}

DHTEntryPointNameResolveCommand::~DHTEntryPointNameResolveCommand()
{
#ifdef ENABLE_ASYNC_DNS
  disableNameResolverCheck(_resolver);
#endif // ENABLE_ASYNC_DNS
}

bool DHTEntryPointNameResolveCommand::execute()
{
  if(_e->_requestGroupMan->downloadFinished() || _e->isHaltRequested()) {
    return true;
  }
  try {
    while(_entryPoints.size()) {
      std::string hostname = _entryPoints.front().first;
      try {
	if(Util::isNumbersAndDotsNotation(hostname)) {
	  std::pair<std::string, uint16_t> p(hostname,
					     _entryPoints.front().second);
	  _resolvedEntryPoints.push_back(p);
	  _entryPoints.erase(_entryPoints.begin());
	  addPingTask(p);
	} else {
	  if(resolveHostname(hostname, _resolver)) {
	    hostname = _resolver->getAddrString();
	    _resolver->reset();
	    std::pair<std::string, uint16_t> p(hostname,
					       _entryPoints.front().second);
	    _resolvedEntryPoints.push_back(p);
	    _entryPoints.erase(_entryPoints.begin());
	    addPingTask(p);
	  } else {
	    _e->commands.push_back(this);
	    return false;
	  }
	}
      } catch(RecoverableException* e) {
	logger->error(EX_EXCEPTION_CAUGHT, e);
	delete e;
	_entryPoints.erase(_entryPoints.begin());
	_resolver->reset();
      }
    }

    if(_bootstrapEnabled && _resolvedEntryPoints.size()) {
      _taskQueue->addPeriodicTask1(_taskFactory->createNodeLookupTask(_localNode->getID()));
      _taskQueue->addPeriodicTask1(_taskFactory->createBucketRefreshTask());
    }
  } catch(RecoverableException* e) {
    logger->error(EX_EXCEPTION_CAUGHT, e);
    delete e;
  }
  return true;
}

void DHTEntryPointNameResolveCommand::addPingTask(const std::pair<std::string, uint16_t>& addr)
{
  SharedHandle<DHTNode> entryNode(new DHTNode());
  entryNode->setIPAddress(addr.first);
  entryNode->setPort(addr.second);
  
  _taskQueue->addPeriodicTask1(_taskFactory->createPingTask(entryNode, 10));
}

bool DHTEntryPointNameResolveCommand::resolveHostname(const std::string& hostname,
						      const NameResolverHandle& resolver)
{
  std::string ipaddr = DNSCacheSingletonHolder::instance()->find(hostname);
  if(ipaddr.empty()) {
#ifdef ENABLE_ASYNC_DNS
    switch(resolver->getStatus()) {
    case NameResolver::STATUS_READY:
      logger->info(MSG_RESOLVING_HOSTNAME, cuid, hostname.c_str());
      resolver->resolve(hostname);
      setNameResolverCheck(resolver);
      return false;
    case NameResolver::STATUS_SUCCESS:
      logger->info(MSG_NAME_RESOLUTION_COMPLETE, cuid,
		   hostname.c_str(), resolver->getAddrString().c_str());
      DNSCacheSingletonHolder::instance()->put(hostname, resolver->getAddrString());
      return true;
      break;
    case NameResolver::STATUS_ERROR:
      throw new DlAbortEx(MSG_NAME_RESOLUTION_FAILED, cuid,
			  hostname.c_str(),
			  resolver->getError().c_str());
    default:
      return false;
    }
#else
    logger->info(MSG_RESOLVING_HOSTNAME, cuid, hostname.c_str());
    resolver->resolve(hostname);
    logger->info(MSG_NAME_RESOLUTION_COMPLETE, cuid,
		 hostname.c_str(), resolver->getAddrString().c_str());
    DNSCacheSingletonHolder::instance()->put(hostname, resolver->getAddrString());
    return true;
#endif // ENABLE_ASYNC_DNS
  } else {
    logger->info(MSG_DNS_CACHE_HIT, cuid,
		 hostname.c_str(), ipaddr.c_str());
    resolver->setAddr(ipaddr);
    return true;
  }
}

#ifdef ENABLE_ASYNC_DNS
void DHTEntryPointNameResolveCommand::setNameResolverCheck(const SharedHandle<NameResolver>& resolver) {
  _e->addNameResolverCheck(resolver, this);
}

void DHTEntryPointNameResolveCommand::disableNameResolverCheck(const SharedHandle<NameResolver>& resolver) {
  _e->deleteNameResolverCheck(resolver, this);
}
#endif // ENABLE_ASYNC_DNS

void DHTEntryPointNameResolveCommand::setBootstrapEnabled(bool f)
{
  _bootstrapEnabled = f;
}

void DHTEntryPointNameResolveCommand::setTaskQueue(const SharedHandle<DHTTaskQueue>& taskQueue)
{
  _taskQueue = taskQueue;
}

void DHTEntryPointNameResolveCommand::setTaskFactory(const SharedHandle<DHTTaskFactory>& taskFactory)
{
  _taskFactory = taskFactory;
}

void DHTEntryPointNameResolveCommand::setRoutingTable(const SharedHandle<DHTRoutingTable>& routingTable)
{
  _routingTable = routingTable;
}

void DHTEntryPointNameResolveCommand::setLocalNode(const SharedHandle<DHTNode>& localNode)
{
  _localNode = localNode;
}

} // namespace aria2
