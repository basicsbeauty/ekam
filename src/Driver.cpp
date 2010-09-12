// ekam -- http://code.google.com/p/ekam
// Copyright (c) 2010 Kenton Varda and contributors.  All rights reserved.
// Portions copyright Google, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of the ekam project nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "Driver.h"

#include <queue>
#include <tr1/memory>
#include <stdexcept>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include "Debug.h"
#include "EventGroup.h"

namespace ekam {

class Driver::ActionDriver : public BuildContext, public EventGroup::ExceptionHandler {
public:
  ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt,
               File* srcfile, Hash srcHash, File* tmploc,
               OwnedPtr<Dashboard::Task>* taskToAdopt);
  ~ActionDriver();

  void start();

  // implements BuildContext -------------------------------------------------------------
  File* findProvider(EntityId id);
  File* findInput(const std::string& basename);

  void provide(File* file, const std::vector<EntityId>& entities);
  void log(const std::string& text);

  void newOutput(const std::string& basename, OwnedPtr<File>* output);

  void addActionType(OwnedPtr<ActionFactory>* factoryToAdopt);

  void passed();
  void failed();

  // implements ExceptionHandler ---------------------------------------------------------
  void threwException(const std::exception& e);
  void threwUnknownException();
  void noMoreEvents();

private:
  class StartCallback : public EventManager::Callback {
  public:
    StartCallback(ActionDriver* actionDriver) : actionDriver(actionDriver) {}
    ~StartCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      actionDriver->asyncCallbackOp.clear();
      actionDriver->action->start(&actionDriver->eventGroup, actionDriver,
                                  &actionDriver->runningAction);
    }

  private:
    ActionDriver* actionDriver;
  };

  class DoneCallback : public EventManager::Callback {
  public:
    DoneCallback(ActionDriver* actionDriver) : actionDriver(actionDriver) {}
    ~DoneCallback() {}

    // implements Callback ---------------------------------------------------------------
    void run() {
      actionDriver->asyncCallbackOp.clear();
      Driver* driver = actionDriver->driver;
      actionDriver->returned();  // may delete actionDriver
      driver->startSomeActions();
    }

  private:
    ActionDriver* actionDriver;
  };


  Driver* driver;
  OwnedPtr<Action> action;
  OwnedPtr<File> srcfile;
  Hash srcHash;
  OwnedPtr<File> tmpdir;
  OwnedPtr<Dashboard::Task> dashboardTask;

  // TODO:  Get rid of "state".  Maybe replace with "status" or something, but don't try to
  //   track both whether we're running and what the status was at the same time.  (I already
  //   had to split isRunning into a separate boolean due to issues with this.)
  enum {
    PENDING,
    RUNNING,
    DONE,
    PASSED,
    FAILED
  } state;

  EventGroup eventGroup;

  StartCallback startCallback;
  DoneCallback doneCallback;
  OwnedPtr<AsyncOperation> asyncCallbackOp;

  bool isRunning;
  OwnedPtr<AsyncOperation> runningAction;

  typedef std::tr1::unordered_map<EntityId, Hash, EntityId::HashFunc> DependencySet;
  DependencySet dependencies;

  OwnedPtrVector<File> outputs;

  struct Provision {
    OwnedPtr<File> file;
    std::vector<EntityId> entities;
  };

  OwnedPtrVector<Provision> provisions;

  void ensureRunning();
  void queueDoneCallback();
  void returned();
  void clearDependencies();

  friend class Driver;
};

Driver::ActionDriver::ActionDriver(Driver* driver, OwnedPtr<Action>* actionToAdopt,
                                   File* srcfile, Hash srcHash, File* tmploc,
                                   OwnedPtr<Dashboard::Task>* taskToAdopt)
    : driver(driver), srcHash(srcHash), state(PENDING), eventGroup(driver->eventManager, this),
      startCallback(this), doneCallback(this), isRunning(false) {
  action.adopt(actionToAdopt);
  srcfile->clone(&this->srcfile);
  tmploc->parent(&this->tmpdir);
  dashboardTask.adopt(taskToAdopt);
}
Driver::ActionDriver::~ActionDriver() {}

void Driver::ActionDriver::start() {
  if (state != PENDING) {
    DEBUG_ERROR << "State must be PENDING here.";
  }
  assert(dependencies.empty());
  assert(outputs.empty());
  assert(provisions.empty());
  assert(!isRunning);

  dependencies.insert(std::make_pair(EntityId::fromFile(srcfile.get()), srcHash));

  state = RUNNING;
  isRunning = true;
  dashboardTask->setState(Dashboard::RUNNING);

  OwnedPtr<EventManager::Callback> callback;
  eventGroup.runAsynchronously(&startCallback, &asyncCallbackOp);
}

File* Driver::ActionDriver::findProvider(EntityId id) {
  ensureRunning();
  EntityMap::const_iterator iter = driver->entityMap.find(id);
  if (iter == driver->entityMap.end()) {
    dependencies.insert(std::make_pair(id, Hash::NULL_HASH));
    return NULL;
  } else {
    dependencies.insert(std::make_pair(id, iter->second.contentHash));
    return iter->second.provider;
  }
}

File* Driver::ActionDriver::findInput(const std::string& basename) {
  ensureRunning();

  OwnedPtr<File> reference;
  tmpdir->relative(basename, &reference);
  return findProvider(EntityId::fromFile(reference.get()));
}

void Driver::ActionDriver::provide(File* file, const std::vector<EntityId>& entities) {
  ensureRunning();

  OwnedPtr<Provision> provision;
  provision.allocate();
  file->clone(&provision->file);
  provision->entities = entities;

  provisions.adoptBack(&provision);
}

void Driver::ActionDriver::log(const std::string& text) {
  ensureRunning();
  dashboardTask->addOutput(text);
}

void Driver::ActionDriver::newOutput(const std::string& basename, OwnedPtr<File>* output) {
  ensureRunning();
  OwnedPtr<File> file;
  tmpdir->relative(basename, &file);
  file->clone(output);

  std::vector<EntityId> entities;
  entities.push_back(EntityId::fromFile(file.get()));
  provide(file.get(), entities);

  outputs.adoptBack(&file);
}

void Driver::ActionDriver::addActionType(OwnedPtr<ActionFactory>* factoryToAdopt) {
  ensureRunning();
  driver->addActionFactory(factoryToAdopt->get());
  driver->rescanForNewFactory(factoryToAdopt->get());
  driver->ownedFactories.adoptBack(factoryToAdopt);
}

void Driver::ActionDriver::noMoreEvents() {
  ensureRunning();

  if (state == RUNNING) {
    state = DONE;
    queueDoneCallback();
  }
}

void Driver::ActionDriver::passed() {
  ensureRunning();

  if (state == FAILED) {
    // Ignore passed() after failed().
    return;
  }

  state = PASSED;
  queueDoneCallback();
}

void Driver::ActionDriver::failed() {
  ensureRunning();

  if (state == FAILED) {
    // Ignore redundant call to failed().
    return;
  } else if (state == DONE) {
    // (done callback should already be queued)
    throw std::runtime_error("Called failed() after success().");
  } else if (state == PASSED) {
    // (done callback should already be queued)
    throw std::runtime_error("Called failed() after passed().");
  } else {
    state = FAILED;
    queueDoneCallback();
  }
}

void Driver::ActionDriver::ensureRunning() {
  if (!isRunning) {
    throw std::runtime_error("Action is not running.");
  }
}

void Driver::ActionDriver::queueDoneCallback() {
  driver->eventManager->runAsynchronously(&doneCallback, &asyncCallbackOp);
}

void Driver::ActionDriver::threwException(const std::exception& e) {
  ensureRunning();
  dashboardTask->addOutput(std::string("uncaught exception: ") + e.what() + "\n");
  asyncCallbackOp.clear();
  state = FAILED;
  returned();
}

void Driver::ActionDriver::threwUnknownException() {
  ensureRunning();
  dashboardTask->addOutput("uncaught exception of unknown type\n");
  asyncCallbackOp.clear();
  state = FAILED;
  returned();
}

void Driver::ActionDriver::returned() {
  ensureRunning();

  // Cancel anything still running.
  runningAction.clear();
  isRunning = false;

  // Pull self out of driver->activeActions.
  OwnedPtr<ActionDriver> self;
  for (int i = 0; i < driver->activeActions.size(); i++) {
    if (driver->activeActions.get(i) == this) {
      driver->activeActions.releaseAndShift(i, &self);
      break;
    }
  }

  // Did the action use any entities that changed since the action started?
  for (DependencySet::const_iterator iter = dependencies.begin();
       iter != dependencies.end(); ++iter) {
    EntityMap::const_iterator iter2 = driver->entityMap.find(iter->first);
    if (iter2 != driver->entityMap.end() && iter2->second.contentHash != iter->second) {
      // Yep.  Must do over.  Clear everything and add again to the action queue.
      state = PENDING;
      provisions.clear();
      outputs.clear();
      dependencies.clear();
      driver->pendingActions.adoptBack(&self);
      dashboardTask->setState(Dashboard::BLOCKED);
      return;
    }
  }

  // Insert self into the completed actions map.
  driver->completedActionPtrs.adopt(this, &self);
  for (DependencySet::const_iterator iter = dependencies.begin();
       iter != dependencies.end(); ++iter) {
    driver->completedActions.insert(std::make_pair(iter->first, this));
  }

  if (state == FAILED) {
    // Failed, possibly due to missing dependencies.
    provisions.clear();
    outputs.clear();
    dashboardTask->setState(Dashboard::BLOCKED);
  } else {
    dashboardTask->setState(state == PASSED ? Dashboard::PASSED : Dashboard::DONE);

    // Register providers.
    for (int i = 0; i < provisions.size(); i++) {
      driver->registerProvider(&provisions.get(i)->file, provisions.get(i)->entities);
    }

    // Enqueue new actions based on output files.
    for (int i = 0; i < outputs.size(); i++) {
      driver->scanForActions(outputs.get(i), outputs.get(i), DERIVED_INPUT);
    }
  }
}

void Driver::ActionDriver::clearDependencies() {
  for (ActionDriver::DependencySet::const_iterator iter = dependencies.begin();
       iter != dependencies.end(); ++iter) {
    std::pair<CompletedActionMap::iterator, CompletedActionMap::iterator>
        range = driver->completedActions.equal_range(iter->first);
    for (CompletedActionMap::iterator iter2 = range.first;
         iter2 != range.second; ++iter2) {
      if (iter2->second == this) {
        driver->completedActions.erase(iter2);
        break;
      }
    }
  }
  dependencies.clear();
}

// =======================================================================================

Driver::Driver(EventManager* eventManager, Dashboard* dashboard, File* src, File* tmp,
               int maxConcurrentActions)
    : eventManager(eventManager), dashboard(dashboard), src(src), tmp(tmp),
      maxConcurrentActions(maxConcurrentActions) {
  if (!tmp->exists()) {
    tmp->createDirectory();
  }
}

Driver::~Driver() {
  // Error out all blocked tasks.
  for (OwnedPtrMap<ActionDriver*, ActionDriver>::Iterator iter(completedActionPtrs); iter.next();) {
    if (iter.key()->state == ActionDriver::FAILED) {
      iter.value()->dashboardTask->setState(Dashboard::FAILED);
    }
  }
}

void Driver::addActionFactory(ActionFactory* factory) {
  actionFactories.push_back(factory);

  std::vector<EntityId> triggerEntities;
  factory->enumerateTriggerEntities(std::back_inserter(triggerEntities));
  for (unsigned int i = 0; i < triggerEntities.size(); i++) {
    triggers.insert(std::make_pair(triggerEntities[i], factory));
  }
}

void Driver::start() {
  scanForActions(src, tmp, ORIGINAL_INPUT);
  startSomeActions();
}

void Driver::startSomeActions() {
  while (activeActions.size() < maxConcurrentActions && !pendingActions.empty()) {
    OwnedPtr<ActionDriver> actionDriver;
    pendingActions.releaseBack(&actionDriver);
    ActionDriver* ptr = actionDriver.get();
    activeActions.adoptBack(&actionDriver);
    try {
      ptr->start();
    } catch (const std::exception& e) {
      ptr->threwException(e);
    } catch (...) {
      ptr->threwUnknownException();
    }
  }
}

void Driver::scanForActions(File* src, File* tmp, ScanType type) {
  OwnedPtrVector<SrcTmpPair> fileQueue;

  {
    OwnedPtr<SrcTmpPair> root;
    root.allocate();
    src->clone(&root->srcFile);
    root->srcFileHash = root->srcFile->contentHash();
    tmp->clone(&root->tmpLocation);
    fileQueue.adoptBack(&root);
  }

  while (!fileQueue.empty()) {
    OwnedPtr<SrcTmpPair> current;
    fileQueue.releaseBack(&current);

    if (current->srcFile->isDirectory()) {
      if (!current->tmpLocation->isDirectory()) {
        current->tmpLocation->createDirectory();
      }

      OwnedPtrVector<File> list;
      current->srcFile->list(list.appender());
      for (int i = 0; i < list.size(); i++) {
        OwnedPtr<SrcTmpPair> newPair;
        newPair.allocate();
        list.release(i, &newPair->srcFile);
        newPair->srcFileHash = newPair->srcFile->contentHash();
        current->tmpLocation->relative(newPair->srcFile->basename(), &newPair->tmpLocation);
        fileQueue.adoptBack(&newPair);
      }
    }

    // Don't allow actions to trigger on the root directory.
    if (current->srcFile->hasParent()) {
      // Poll all ActionFactories to see if they like this file.
      for (unsigned int i = 0; i < actionFactories.size(); i++) {
        ActionFactory* factory = actionFactories[i];
        OwnedPtr<Action> action;
        if (factory->tryMakeAction(current->srcFile.get(), &action)) {
          queueNewAction(&action, current->srcFile.get(), current->srcFileHash,
                         current->tmpLocation.get());
        }
      }

      // Register the file as an entity.  We only do this for original inputs because derived
      // files are registered by the creating action.
      if (type == ORIGINAL_INPUT) {
        OwnedPtr<File> fileClone;
        current->srcFile->clone(&fileClone);
        std::vector<EntityId> entities;
        entities.push_back(EntityId::fromFile(fileClone.get()));
        registerProvider(&fileClone, entities);
      }

      // Add to allScannedFiles for easy re-scanning later.
      allScannedFiles.adoptBack(&current);
    }
  }
}

void Driver::rescanForNewFactory(ActionFactory* factory) {
  for (int i = 0; i < allScannedFiles.size(); i++) {
    OwnedPtr<Action> action;
    if (factory->tryMakeAction(allScannedFiles.get(i)->srcFile.get(), &action)) {
      const SrcTmpPair& srctmp = *allScannedFiles.get(i);
      queueNewAction(&action, srctmp.srcFile.get(), srctmp.srcFileHash, srctmp.tmpLocation.get());
    }
  }
}

void Driver::queueNewAction(OwnedPtr<Action>* actionToAdopt, File* file,
                            const Hash& fileHash, File* tmpLocation) {
  OwnedPtr<Dashboard::Task> task;
  dashboard->beginTask((*actionToAdopt)->getVerb(), file->canonicalName(), &task);

  OwnedPtr<ActionDriver> actionDriver;
  actionDriver.allocate(this, actionToAdopt, file, fileHash, tmpLocation, &task);

  pendingActions.adoptBack(&actionDriver);
}

void Driver::registerProvider(OwnedPtr<File>* fileToAdopt, const std::vector<EntityId>& entities) {
  File* file = fileToAdopt->get();
  Hash hash = file->contentHash();
  filePtrs.adopt(file, fileToAdopt);

  for (std::vector<EntityId>::const_iterator iter = entities.begin();
       iter != entities.end(); ++iter) {
    const EntityId& entity = *iter;
    EntityInfo info = {file, hash};
    entityMap[entity] = info;

    resetDependentActions(entity);

    fireTriggers(entity, file, info.contentHash);
  }
}

void Driver::resetDependentActions(const EntityId& entity) {
  std::tr1::unordered_set<EntityId, EntityId::HashFunc> entitiesToReset;
  entitiesToReset.insert(entity);

  while (!entitiesToReset.empty()) {
    EntityId currentEntity = *entitiesToReset.begin();
    entitiesToReset.erase(entitiesToReset.begin());

    std::pair<CompletedActionMap::const_iterator, CompletedActionMap::const_iterator>
        range = completedActions.equal_range(currentEntity);
    int pendingActionsPos = pendingActions.size();
    for (CompletedActionMap::const_iterator iter = range.first; iter != range.second; ++iter) {
      // Reset action to pending.

      OwnedPtr<ActionDriver> action;
      if (completedActionPtrs.release(iter->second, &action)) {
        action->state = ActionDriver::PENDING;

        // All of this action's outputs must also be reset.
        for (int i = 0; i < action->provisions.size(); i++) {
          const std::vector<EntityId>& subEntities = action->provisions.get(i)->entities;
          entitiesToReset.insert(subEntities.begin(), subEntities.end());
        }

        action->provisions.clear();
        action->outputs.clear();
        pendingActions.adoptBack(&action);
      } else {
        DEBUG_ERROR << "ActionDriver in completedActions but not completedActionPtrs?";
      }
    }

    for (; pendingActionsPos < pendingActions.size(); pendingActionsPos++) {
      pendingActions.get(pendingActionsPos)->clearDependencies();
    }

    if (completedActions.find(currentEntity) != completedActions.end()) {
      DEBUG_ERROR << "clearDependencies() didn't work?";
    }
  }
}

void Driver::fireTriggers(const EntityId& entity, File* file, const Hash& fileHash) {
  std::pair<TriggerMap::const_iterator, TriggerMap::const_iterator>
      range = triggers.equal_range(entity);
  for (TriggerMap::const_iterator iter = range.first; iter != range.second; ++iter) {
    OwnedPtr<Action> triggeredAction;
    if (iter->second->tryMakeAction(entity, file, &triggeredAction)) {
      queueNewAction(&triggeredAction, file, fileHash, file);
    }
  }
}

}  // namespace ekam
