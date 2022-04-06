/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2022 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "mcts/search.h"

#include <algorithm>
#include <boost/process.hpp>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>

#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/encoder.h"
#include "utils/fastmath.h"
#include "utils/random.h"

std::default_random_engine generator;
std::uniform_real_distribution<float> distribution(1,0);

namespace lczero {

void Search::OpenAuxEngine() REQUIRES(threads_mutex_) {
  if (params_.GetAuxEngineFile() == "") return;
  for(int i = 0; i < params_.GetAuxEngineInstances(); i++){
    auxengine_threads_.emplace_back([this]() { AuxEngineWorker(); });
  }
}

// void SearchWorker::AuxMaybeEnqueueNode(Node* n, int source) {
void SearchWorker::AuxMaybeEnqueueNode(Node* n) {
  // the caller (DoBackupUpdate()->DoBackupUpdateSingleNode()) has a lock on search_->nodes_mutex_, so no other thread will change n right now.
  // There are two callers, also PreExtend() which also has that lock

  // Since we take a lock below, have to check if search is stopped.
  // No, the real reason is that we must not add nodes after purging has started.
  if (search_->stop_.load(std::memory_order_acquire)){
    return;
  }

  search_->search_stats_->auxengine_mutex_.lock();

  search_->number_of_times_called_AuxMaybeEnqueueNode_ += 1; // only for stats, not functionally necessary.
  
  // if purging has already happened, then do nothing
  if(! search_->search_stats_->final_purge_run) {
    // if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE
    //   << "AuxMaybeEnqueueNode() picked node: " << n->DebugString() 
    //   << " for the persistent_queue_of_nodes which has size: "
    //   << search_->search_stats_->persistent_queue_of_nodes.size()
    //   << " The source was " << source;
    n->SetAuxEngineMove(0xfffe); // magic for pending
    if(search_->search_stats_->persistent_queue_of_nodes.size() < 15000) { // safety net for too low values of AuxEngineThreshold, which would cause this queue to overflow somehow, or just take too much time to check between moves.
      search_->search_stats_->persistent_queue_of_nodes.push(n);
      // search_->search_stats_->source_of_queued_nodes.push(source);
      search_->auxengine_cv_.notify_one();
    }
  }
  search_->search_stats_->auxengine_mutex_.unlock();
}

void Search::AuxEngineWorker() {

  // aquire a lock on pure_stats_mutex_ to ensure no other thread is
  // modifying search_stats_->thread_counter or the vector_of_*
  // vectors

  search_stats_->pure_stats_mutex_.lock();

  // Find out which thread we are by reading the thread_counter.

  // Don't increment the thread_counter before all global vectors are
  // initiated, or MaybeTriggerStop() in search.cc will try to write
  // to uninitiated adresses.

  long unsigned int our_index = search_stats_->thread_counter;

  // if our_index is greater than the size of the vectors then we know for sure we must start/initiate everything.
  // if our_index + 1 is equal to, or smaller than the size of the vectors then we can safely check search_stats_->vector_of_auxengine_ready_[our_index] and act if it is false

  if(our_index + 1 > search_stats_->vector_of_auxengine_ready_.size() ||
     (
      our_index + 1 <= search_stats_->vector_of_auxengine_ready_.size() &&
      ! search_stats_->vector_of_auxengine_ready_[our_index]
     )
   ) {

    // increase the thread_counter.
    search_stats_->thread_counter++;
 
    // populate the global vectors. 
    search_stats_->vector_of_ipstreams.emplace_back(new boost::process::ipstream);
    search_stats_->auxengine_stopped_mutex_.lock();
    search_stats_->vector_of_opstreams.emplace_back(new boost::process::opstream);
    search_stats_->auxengine_stopped_mutex_.unlock();
    search_stats_->vector_of_children.emplace_back(new boost::process::child);

    // Start the helper
    *search_stats_->vector_of_children[our_index] = boost::process::child(params_.GetAuxEngineFile(), boost::process::std_in < *search_stats_->vector_of_opstreams[our_index], boost::process::std_out > *search_stats_->vector_of_ipstreams[our_index]);

    // Record that we have started, so that we can skip this on the next invocation.
    search_stats_->vector_of_auxengine_ready_.push_back(true);

    // unlock while we wait for the engine to be finished?
    search_stats_->pure_stats_mutex_.unlock();

    search_stats_->auxengine_stopped_mutex_.lock();
    search_stats_->auxengine_stopped_.push_back(true);
    search_stats_->auxengine_stopped_mutex_.unlock();

    std::string bar;
    // If AuxEngineOptionsOnRoot is set, Thread zero (and one if it exists) uses a different parameter and it continuosly explores root node only.
    // If not set, thread zero becomes just another in-tree helper instance.
    if(our_index < 2 &&
       !params_.GetAuxEngineOptionsOnRoot().empty()
       ){
      bar = params_.GetAuxEngineOptionsOnRoot();
    } else {
      // in-tree time based evaluations
      bar = params_.GetAuxEngineOptions();
    }
    
    std::istringstream iss(bar);
    std::string token;
    while(std::getline(iss, token, '=')) {
      std::ostringstream oss;
      oss << "setoption name " << token;
      std::getline(iss, token, ';');
      oss << " value " << token;
      search_stats_->auxengine_stopped_mutex_.lock();
      *search_stats_->vector_of_opstreams[our_index] << oss.str() << std::endl;
      search_stats_->auxengine_stopped_mutex_.unlock();	
    }
    search_stats_->auxengine_stopped_mutex_.lock();
    *search_stats_->vector_of_opstreams[our_index] << "uci" << std::endl;
    search_stats_->auxengine_stopped_mutex_.unlock();      

    std::string line;
    while(std::getline(*search_stats_->vector_of_ipstreams[our_index], line)) {
      if (params_.GetAuxEngineVerbosity() >= 10) LOGFILE << line;
      std::istringstream iss(line);
      std::string token;
      iss >> token >> std::ws;
      if (token == "uciok") {
        break;
      } else if (token == "option") {
        iss >> token >> std::ws;
        if (token == "name") {
          iss >> token >> std::ws;
          if (token == "SyzygyPath" && syzygy_tb_) {
            std::ostringstream oss;
            oss << "setoption name SyzygyPath value " << syzygy_tb_->get_paths();
            if (params_.GetAuxEngineVerbosity() >= 10) LOGFILE << oss.str();
	    search_stats_->auxengine_stopped_mutex_.lock();
	    *search_stats_->vector_of_opstreams[our_index] << oss.str() << std::endl;	    
	    search_stats_->auxengine_stopped_mutex_.unlock();
          }
        }
      }
    }
    
    if(our_index == 0){
      search_stats_->pure_stats_mutex_.lock();
      // Initiate some stats and parameters (Threshold needs to be set
      // earlier, see search() in search.cc)
      search_stats_->AuxEngineTime = params_.GetAuxEngineTime();
      search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
      search_stats_->Total_number_of_nodes = 0;
      // search_stats_->initial_purge_run = true;
      search_stats_->my_pv_cache_mutex_.lock();      
      search_stats_->my_pv_cache_.clear(); // Clear the PV cache.
      search_stats_->my_pv_cache_mutex_.unlock();      
      if(search_stats_->New_Game){
	search_stats_->New_Game = false;
	// Automatically inactivate the queueing machinery if there is only one instance AND OptionsOnRoot is NON-empty. Could save some time in ultra-bullet.
	if(params_.GetAuxEngineInstances() == 1 &&
	   !params_.GetAuxEngineOptionsOnRoot().empty()
	   ){
	  search_stats_->AuxEngineThreshold = 0;
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Inactivating the queueing machinery since there is exactly one instance and OnRoot is non-empty.";
	} else  {
	  search_stats_->AuxEngineThreshold = params_.GetAuxEngineThreshold();
	}
      }
    }
  } else {

    bool needs_to_purge_nodes = true;
    bool needs_to_purge_PVs = true;
    if(search_stats_->initial_purge_run){
      needs_to_purge_nodes = false;
      needs_to_purge_PVs = false;
    }

    if(our_index == 0 && search_stats_->New_Game){

      needs_to_purge_nodes  = false;
      needs_to_purge_PVs  = false;
	
      search_stats_->AuxEngineTime = params_.GetAuxEngineTime();
      // Automatically inactivate the queueing machinery if there is only one instance and OptionsOnRoot is non-empty. Could save some time in ultra-bullet.
      if(params_.GetAuxEngineInstances() == 1 &&
	 !params_.GetAuxEngineOptionsOnRoot().empty()
	 ){
	search_stats_->AuxEngineThreshold = 0;
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Inactivating the queueing machinery since there is exactly one instance and OnRoot is non-empty.";
      } else  {
	search_stats_->AuxEngineThreshold = params_.GetAuxEngineThreshold();
      }

      search_stats_->best_move_candidates_mutex.lock();
      search_stats_->winning_ = false;
      bool reconfiguration_needed = search_stats_->winning_threads_adjusted;
      search_stats_->winning_threads_adjusted = false;
      search_stats_->number_of_nodes_in_support_for_helper_eval_of_root = 0;
      search_stats_->number_of_nodes_in_support_for_helper_eval_of_leelas_preferred_child_of_root = 0;
      search_stats_->best_move_candidates_mutex.unlock();
      if(reconfiguration_needed){
	// during the previous game, the root exploring helper was reconfigured to use more threads, reconfigure again back to the normal state.
	// if winning_ was changed from false to true only during the very last move, winning_threads_adjusted is false and no reconfiguration has yet taken place, thus no reconfiguration is needed here.
	if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxWorker() reconfigured the root-helper to use " << search_stats_->non_winning_root_threads_ << " number of threads again since a new game started.";
	search_stats_->auxengine_stopped_mutex_.lock();
	*search_stats_->vector_of_opstreams[our_index] << "setoption name Threads value " << search_stats_->non_winning_root_threads_ << std::endl;	    
	search_stats_->auxengine_stopped_mutex_.unlock();
      }

      search_stats_->Total_number_of_nodes = 0;
      search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
      search_stats_->size_of_queue_at_start = 0;

      search_stats_->New_Game = false;

      // change lock to purge queue of PVs
      // search_stats_->pure_stats_mutex_.unlock();
      search_stats_->fast_track_extend_and_evaluate_queue_mutex_.lock();
      search_stats_->fast_track_extend_and_evaluate_queue_ = {};
      search_stats_->fast_track_extend_and_evaluate_queue_mutex_.unlock();

      // different lock for queue of nodes
      search_stats_->auxengine_mutex_.lock();
      search_stats_->persistent_queue_of_nodes = {};
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Thread 0 has initiated the global variables, since a new game has started.";
      search_stats_->auxengine_mutex_.unlock();
    }

    // If the helper now claims a win, reroute all resources to the root explorer.
    int threads_when_winning = 0;

    search_stats_->best_move_candidates_mutex.lock();
    bool winning_threads_adjusted = search_stats_->winning_threads_adjusted; // temporary variable need to avoid nested locks below.
    
    if(search_stats_->winning_){
      std::string bar;
      // If AuxEngineOptionsOnRoot is set, Thread zero uses a different parameter and it continuosly explores root node only.
      // If not set, thread zero becomes just another in-tree helper instance.
      if(our_index == 0 &&
	 !params_.GetAuxEngineOptionsOnRoot().empty()
	 ){
	bar = params_.GetAuxEngineOptionsOnRoot();
      } else {
	// in-tree time based evaluations
	bar = params_.GetAuxEngineOptions();
      }
      std::string temp_threads_when_winning;
      std::istringstream options_stream(bar);
      std::string pair_string;
      std::string option_name;
      while(std::getline(options_stream, option_name, ';')) {
	if(option_name.substr(0, 8) == "Threads=") {
	  std::istringstream my_stream(option_name);
	  std::getline(my_stream, temp_threads_when_winning, '=');
	  std::getline(my_stream, temp_threads_when_winning, '=');
	  threads_when_winning = stoi(temp_threads_when_winning);
	  search_stats_->non_winning_root_threads_ = threads_when_winning;
	}
      }
      threads_when_winning = 2 * threads_when_winning + params_.GetAuxEngineInstances();
    }

    search_stats_->best_move_candidates_mutex.unlock();

    // if threads_when_winning > 0, then reconfigure the helper managed by thread 0, and all other threads should just return early (doing nothing)
    if(threads_when_winning > 0){
      if(our_index > 0){
	// LOGFILE << "AuxWorker() thread " << our_index << " shutting down since the helper claims a win";
	search_stats_->pure_stats_mutex_.unlock();
	return;
      } else {
	if(!winning_threads_adjusted){
	  // Thread zero, just reconfigure the root explorer to use all available threads
	  search_stats_->best_move_candidates_mutex.lock();
	  search_stats_->winning_threads_adjusted = true;
	  search_stats_->best_move_candidates_mutex.unlock();	  
	  LOGFILE << "AuxWorker() reconfigured the root-helper to use " << threads_when_winning << " threads.";
	  search_stats_->auxengine_stopped_mutex_.lock();
	  *search_stats_->vector_of_opstreams[our_index] << "setoption name Threads value " << threads_when_winning << std::endl;	    
	  search_stats_->auxengine_stopped_mutex_.unlock();
	}
      }
    }
    

    // AuxEngine(s) were already started. If we are thread zero then (1) Purge the queue(s) and (2) kickstart root if the queue is empty and root has edges.
    // If another thread 0 purge and exit before we got started, we can be thread zer0 now.
    search_stats_->thread_counter++;

    if(our_index == 0){

      if(needs_to_purge_nodes || needs_to_purge_PVs){
	search_stats_->auxengine_mutex_.lock();
	if(search_stats_->persistent_queue_of_nodes.size() > 0){
	  // The even elements are the actual nodes, the odd elements is root if the preceding even element is still a relevant node.
	  int number_of_nodes_before_purging = int(search_stats_->size_of_queue_at_start / 2);
	  std::queue<Node*> persistent_queue_of_nodes_temp_;
	  for(int i=0; i < search_stats_->size_of_queue_at_start; i = i + 2){
	    Node * n = search_stats_->persistent_queue_of_nodes.front();
	    search_stats_->persistent_queue_of_nodes.pop();
	    Node * n_parent = search_stats_->persistent_queue_of_nodes.front();
	    search_stats_->persistent_queue_of_nodes.pop();
	    if(n_parent == root_node_){
	      // node is still relevant
	      persistent_queue_of_nodes_temp_.push(n);
	    }
	  }
	  // update search_stats_->persistent_queue_of_nodes
	  int my_size = persistent_queue_of_nodes_temp_.size();
	  for(int i=0; i < my_size; i++){      
	    search_stats_->persistent_queue_of_nodes.push(persistent_queue_of_nodes_temp_.front());
	    persistent_queue_of_nodes_temp_.pop();
	  }
	  if (params_.GetAuxEngineVerbosity() >= 3)
	    LOGFILE << "Purged " << number_of_nodes_before_purging - search_stats_->persistent_queue_of_nodes.size()
		    << " nodes from the query queue due to the move selected by the opponent. " << search_stats_->persistent_queue_of_nodes.size()
		    << " nodes remain in the queue.";
	}

	// Also purge obsolete PV:s if any, but that requires a different lock
	search_stats_->auxengine_mutex_.unlock();
	search_stats_->fast_track_extend_and_evaluate_queue_mutex_.lock();

	// Should be safe and should not require a lock, as long as solidtrees is inactivated.
	bool root_valid_move_found = false;
	Move valid_move = root_node_->GetOwnEdge()->GetMove();
	root_valid_move_found = true;
	
	if(root_valid_move_found &&
	   search_stats_->fast_track_extend_and_evaluate_queue_.size() > 0){
	  std::queue<std::vector<Move>> fast_track_extend_and_evaluate_queue_temp_;
	  long unsigned int my_size = search_stats_->fast_track_extend_and_evaluate_queue_.size();
	  while(search_stats_->fast_track_extend_and_evaluate_queue_.size() > 0){
	    std::vector<Move> pv = search_stats_->fast_track_extend_and_evaluate_queue_.front();
	    search_stats_->fast_track_extend_and_evaluate_queue_.pop();
	    if(pv.size() > 1){
	      if(pv[0] == valid_move){
		// remove the first move, which is the move the opponent made that lead to the current position
		pv.erase(pv.begin());
		fast_track_extend_and_evaluate_queue_temp_.push(pv);
	      }
	    } else {
	      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxEngineWorker() found PV of size less than 2, discarding it." << pv.size();		  
	    }
	  }
	  // Empty the queue and copy back the relevant ones.
	  search_stats_->fast_track_extend_and_evaluate_queue_ = {};
	  long unsigned int size_kept = fast_track_extend_and_evaluate_queue_temp_.size();
	  for(long unsigned int i=0; i < size_kept; i++){
	    search_stats_->fast_track_extend_and_evaluate_queue_.push(fast_track_extend_and_evaluate_queue_temp_.front());
	    fast_track_extend_and_evaluate_queue_temp_.pop();
	  }
	  if (params_.GetAuxEngineVerbosity() >= 4)	  
	    LOGFILE << "Purged " << my_size - size_kept << " PVs due to the move selected by the opponent. " << size_kept
		    << " PVs remain in the queue.";
	}
	
	search_stats_->fast_track_extend_and_evaluate_queue_mutex_.unlock();
      } // end of needs_to_purge*
    } else {
      // We are not thread zero so just release the lock after we have increased the thread counter.
      search_stats_->pure_stats_mutex_.unlock();
    }
  } // Not starting from scratch

  // at this point thread zero has pure_stats_mutex, the other threads has no lock.
    
  Node* n;
  bool not_yet_notified = true;
  bool root_is_queued = false;
  int thread_counter; // to store the number of remaining threads when we exit.

  // let only thread 0 check of root has edges, other threads wait for initial_purge_run before they start.
  // Perhaps we are not the first thread 0 so check for search_stats_->initial_purge_run
  if(our_index == 0 && ! search_stats_->initial_purge_run){

    // This acts like a signal for other threads that they can start
    // working. PreExt..() also needs at least a shared lock on
    // pure_stats_mutex to get going.

    if(not_yet_notified){
      // Do this once only.
      search_stats_->initial_purge_run = true;
      // if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxEngineWorker() thread 0 have set initial_purge_run and is about to release shared lock pure_stats_mutex_.";
      search_stats_->pure_stats_mutex_.unlock();
      not_yet_notified = false;	
    }

    // if we are thread zero, don't read from the queue, just take the root node as soon as it got edges.
    // kickstart with the root node, no need to wait for it to get some
    // amount of visits. Except if root is not yet expanded, or lacks
    // edges for any other reason (e.g. being terminal), in which case
    // we should wait and try again later.

    while(!root_is_queued) {
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread 0 about to aquire a shared lock nodes_mutex_ in order to read root";
      nodes_mutex_.lock_shared(); // only needed to read GetNumEdges(), SetAuxEngineMove(0xfffe) is already protected by search_stats_->auxengine_mutex_.lock();
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread 0 aquired a shared lock nodes_mutex_ in order to read root";      
      if(root_node_->GetNumEdges() > 0){
	// root is extended.
	nodes_mutex_.unlock_shared(); // unlock the read-lock on noodes.
	
	search_stats_->auxengine_mutex_.lock();
	root_node_->SetAuxEngineMove(0xfffe); // mark root as pending and queue it
	search_stats_->auxengine_mutex_.unlock();

	if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxEngineWorker() thread 0 found edges on root, allowed other threads to enter their main loop by setting initial_purge_run, and, finally, released shared lock nodes_mutex_.";
	// always throw root at thread 0, even if there is no special root explorer, ie if params_.GetAuxEngineOptionsOnRoot().empty()
	root_is_queued = true;
	DoAuxEngine(root_node_, our_index);
      } else {
	nodes_mutex_.unlock_shared(); // unlock, nothing more to do until root gets edges.
	if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxEngineWorker() thread 0 released shared lock nodes_mutex_.";	
	if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "AuxEngineWorker() thread 0 found root node has no edges will sleep 30 ms";
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(30ms);
      }
    } // end of while not root is queued.
  }

  while (!stop_.load(std::memory_order_acquire)) {
    while(not_yet_notified){
      // Only check this until it has passed once.
      // Wait for search_stats_->initial_purge_run == true before starting to work.
      // if search has already stopped, break early.
      if (stop_.load(std::memory_order_acquire)) break;
      bool initial_purge_run;
      {
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread " << our_index << " trying to obtain a shared lock on search_stats_->pure_stats_mutex_.";
	// SharedMutex::Lock lock(search_stats_->pure_stats_mutex_);	  
	// std::shared_lock lock(search_stats_->pure_stats_mutex_);
	std::unique_lock lock(search_stats_->pure_stats_mutex_);
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread " << our_index << " obtained a shared lock on search_stats_->pure_stats_mutex_.";
	initial_purge_run = search_stats_->initial_purge_run;
      }
      if(!initial_purge_run) {
	// search_stats_->pure_stats_mutex_.unlock();
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread " << our_index << " waiting for thread 0 to purge the queues and check that root has edges, will sleep in cycles of 10 ms until that happens or search is stopped.";
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(30ms);
      } else {
	// // purge is done, just release the lock.
	// search_stats_->pure_stats_mutex_.unlock();
	// OK, we are good to go.
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread: " << our_index << " ready to enter the main loop.";
	not_yet_notified = false; // never check again.
      }
    }

    // You may only listen if you have this lock: auxengine_listen_mutex_ this way we avoid spurios awakenings.
    search_stats_->auxengine_listen_mutex_.lock();

    // This is the main loop for in-tree helpers. Before trying to get locks to enter it, check if search has not already been stopped.
    if (stop_.load(std::memory_order_acquire)) {
      // LOGFILE << "AuxWorker(), thread " << our_index << " breaking the main loop because search is (already) stopped.";
      search_stats_->auxengine_listen_mutex_.unlock();	
      break;
    }

    {
      // std::unique_lock<std::mutex> lock(search_stats_->auxengine_mutex_);
      std::unique_lock lock(search_stats_->auxengine_mutex_);
      // Wait until there's some work to compute.
      if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "AuxWorker(), thread " << our_index << " has the unique lock on auxengine_mutex_ waiting for work.";	
      auxengine_cv_.wait(lock, [&] { return stop_.load(std::memory_order_acquire) || !search_stats_->persistent_queue_of_nodes.empty(); });
      // auxengine_cv_.wait(lock, [&] { return stop_.load(std::memory_order_acquire); });	
      // at this point, the lock is released and aquired again, which is why we want the outer lock, without which another thread could intercept us here.
      if (stop_.load(std::memory_order_acquire)) {
	search_stats_->auxengine_listen_mutex_.unlock();
	break;
      }
      if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "AuxWorker(), thread " << our_index << " got work.";
      if (search_stats_->persistent_queue_of_nodes.size() > 0){
	n = search_stats_->persistent_queue_of_nodes.front();
	search_stats_->persistent_queue_of_nodes.pop();
      } else {
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWorker(), thread " << our_index << " someone robbed us on our node!";	  
      }
    } // implictly release the lock on search_stats_->auxengine_mutex_
    search_stats_->auxengine_listen_mutex_.unlock();
    DoAuxEngine(n, our_index);
  } // end of while loop

  if(stop_.load(std::memory_order_acquire)){
    // We are thread 0 and not_yet_notified is true, then someone else was thread 0 before us, and we still have the lock on pure_stats, which is a good thing.
    // This construct should solve https://github.com/hans-ekbrand/lc0/issues/13
    if(our_index == 0 && not_yet_notified){
      search_stats_->thread_counter--;
      thread_counter = search_stats_->thread_counter;      
      search_stats_->pure_stats_mutex_.unlock();
    } else {
      // The normal scenario, need to grab the lock
      {
	std::unique_lock lock(search_stats_->pure_stats_mutex_);
	search_stats_->thread_counter--;
	thread_counter = search_stats_->thread_counter;
      }
    }
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker thread " << our_index << " done. The thread counter is now " << thread_counter;
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWorker(), thread " << our_index << " released the lock on pure_stats.";
    // Almost always log the when the last thread exits.
    if(thread_counter == 0 && params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "All AuxEngineWorker threads are now idle";
  }
}

  void Search::AuxEncode_and_Enqueue(std::string pv_as_string, int depth, ChessBoard my_board, Position my_position, std::vector<lczero::Move> my_moves_from_the_white_side, bool require_some_depth, int thread) {
  // Take a string recieved from a helper engine, turn it into a vector with elements of type Move and queue that vector.

 //  // Quit early if search has stopped
 // if(stop_.load(std::memory_order_acquire)) {
 //   // if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Would have quit early from AuxEncode_and_Enqueue() since search has stopped, but decided to take the risk and go on.";   
 //   if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread " << thread << ": Quitting early from AuxEncode_and_Enqueue() since search has stopped.";
 //   return;
 // }

  std::istringstream iss(pv_as_string);  
  std::string pv;
  std::vector<uint16_t> pv_moves;

  std::string s = "position fen " + GetFen(my_position); // for informational purposes only.
  std::string token;

  // To get the moves in UCI format, we have to construct a board, starting from root and then apply the moves.
  // Traverse up to root, and store the moves in a vector.
  // When we internally use the moves to extend nodes in the search tree, always use move as seen from the white side.
  // Apply the moves in reversed order to get the proper board state from which we can then make moves in legacy format.
  // std::vector<lczero::Move> my_moves;
  // std::vector<lczero::Move> my_moves_from_the_white_side;  
    
  bool flip = played_history_.IsBlackToMove() ^ (depth % 2 == 0);

  // auto bestmove_packed_int = Move(token, !flip).as_packed_int();
  // depth is distance between root and the starting point for the
  // auxengine.
  // depth_reached records the depth the helper claim to have search.
  // The PV is capped at this length (and can be shortened again in PreExt..()

  int pv_length = 1;
  int depth_reached = 0;
  int nodes_to_support = 0;
  int max_pv_length = 99; // Dirty work around for too many levels of recursion.
  int eval;
  bool winning = false;

  while(iss >> pv >> std::ws) {
    if (pv == "info"){
      continue;
    }
    if (pv == "string" || pv == "currmove"){
      // not for us.
      return;
    }
    if (pv == "depth") {
      // Figure out which depth was reached (can be zero).
      iss >> depth_reached >> std::ws;
    }
    if (pv == "cp") {
      iss >> eval >> std::ws;
      if(depth == 0){
	search_stats_->best_move_candidates_mutex.lock();
	search_stats_->helper_eval_of_root = eval;
	search_stats_->best_move_candidates_mutex.unlock();	
      }
      if(thread == 1){ // assume thread 1 works with leelas preferred child of root.
	search_stats_->best_move_candidates_mutex.lock();	
	search_stats_->helper_eval_of_leelas_preferred_child_of_root = -eval;
	search_stats_->best_move_candidates_mutex.unlock();	
      }
      if(depth == 0 && eval > 250) {
	winning = true;
      }
    }
    if (pv == "nodes") {
      // Figure out how many nodes this PV is based on.
      iss >> nodes_to_support >> std::ws;
      // Save time by ignoring PVs with low support.
      // if(nodes_to_support < 10000){
      // 	return;
      // }
    }

    // Either "don't require depth" or depth > 14 or at least 10000 nodes
    if (pv == "pv" && (!require_some_depth || nodes_to_support >= 10000 || depth_reached > 14)) {
      while(iss >> pv >> std::ws &&
	    pv_length < depth_reached &&
	    pv_length < max_pv_length) {
	Move m;
	if (!Move::ParseMove(&m, pv, !flip)) {	
	  if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Ignoring bad pv move: " << pv;
	  break;
	  // why not return instead of break?
	}
	// m is always from the white side, pv is not. No need to mirror the board then? Actually, yes.
	// convert to Modern encoding, update the board and the position

	Move m_in_modern_encoding = my_board.GetModernMove(m);
	my_moves_from_the_white_side.push_back(m_in_modern_encoding); // Add the PV to the queue 
	pv_moves.push_back(m_in_modern_encoding.as_packed_int());
	my_position = Position(my_position, m_in_modern_encoding);	
	my_board.ApplyMove(m_in_modern_encoding);
	my_board.Mirror();	

	flip = !flip;
	pv_length++;
      }
    }
  }

  // Too short PV are probably not reliable (> 4 seems to suffice), too high bar can be bad with low values of AuxEngineTime
  // perhaps speed will be improved if we ignore the very short PVs?
  // const long unsigned int min_pv_size = 5;
  const long unsigned int min_pv_size = 6;
  if (pv_moves.size() >= min_pv_size){

    // check if the PV is new
    std::ostringstream oss;
    // Convert all but the last element to avoid a trailing "," https://stackoverflow.com/questions/8581832/converting-a-vectorint-to-string
    std::copy(pv_moves.begin(), pv_moves.end()-1, std::ostream_iterator<int>(oss, ","));
    // Now add the last element with no delimiter
    oss << pv_moves.back();
    // TODO protect the PV cache with a mutex? Stockfish does not, and worst case scenario is that the same PV is sent again, so probably not needed.
    // https://stackoverflow.com/questions/8581832/converting-a-vectorint-to-string
    search_stats_->my_pv_cache_mutex_.lock();
    if ( search_stats_->my_pv_cache_.find(oss.str()) == search_stats_->my_pv_cache_.end() ) {
      if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "string not found in the cache, adding it.";
      search_stats_->my_pv_cache_[oss.str()] = true;
    } else {
      if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "string found in the cache. Return early.";
      search_stats_->my_pv_cache_mutex_.unlock();
      return;
    }
    search_stats_->my_pv_cache_mutex_.unlock();
    
    if (params_.GetAuxEngineVerbosity() >= 9){
      std::string debug_string;
      // No lock required here, my_moves_from_the_white_side is only a simple queue of Moves, it has nothing to do with the searchtree.
      for(int i = 0; i < (int) my_moves_from_the_white_side.size(); i++){
	debug_string = debug_string + my_moves_from_the_white_side[i].as_string() + " ";
      }
      if(played_history_.IsBlackToMove()){
	LOGFILE << "debug info: length of PV given to helper engine: " << depth << " position given to helper: " << s << " black to move at root, length of my_moves_from_the_white_side " << my_moves_from_the_white_side.size() << " my_moves_from_the_white_side: " << debug_string;
      } else {
	LOGFILE << "debug info: length of PV given to helper engine: " << depth << " position given to helper: " << s << " white to move at root, length of my_moves_from_the_white_side " << my_moves_from_the_white_side.size() << " my_moves_from_the_white_side: " << debug_string;
      }
    }

    if(depth == 0){
      // show the PV from root from the helper
      std::string debug_string_root;      
      for(int i = 0; i < (int) my_moves_from_the_white_side.size(); i++){
	debug_string_root = debug_string_root + my_moves_from_the_white_side[i].as_string() + " ";
      }
      if(params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "PV from the root explorer: " << debug_string_root;
    }

    if(stop_.load(std::memory_order_acquire) && depth == 0){
      // final eval from the root-explorer
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Final eval from the root explorer: " << eval << " with the move " << my_moves_from_the_white_side.front().as_string();      
    }

    // Prepare autopilot and blunder vetoing START
    // before search_stats_->winning_threads_adjusted is set, accept changes in all directions.
    // after search_stats_->winning_threads_adjusted is set, just inform, don't change the state of search_stats_->winning_
    search_stats_->best_move_candidates_mutex.lock();
    if(depth == 0){
      // Make it easy for thread 1 to find the divergence
      search_stats_->helper_PV = my_moves_from_the_white_side;
      search_stats_->auxengine_stopped_mutex_.lock();
      if(!search_stats_->auxengine_stopped_[1]){
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "SendUciInfo() Stopping the A/B helper Start for thread=" << 1 << " Start.";
	*search_stats_->vector_of_opstreams[1] << "stop" << std::endl; // stop the A/B helper
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "SendUciInfo() Stopping the A/B helper for thread=" << 1 << " Stop.";
	search_stats_->auxengine_stopped_[1] = true;
      }
      search_stats_->auxengine_stopped_mutex_.unlock();
    }

    if (winning && !search_stats_->winning_){
      if(!search_stats_->winning_threads_adjusted){
	search_stats_->winning_ = true;
      }
      search_stats_->winning_move_ = my_moves_from_the_white_side.front();
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "The helper engine thinks the root position is winning: cp = " << eval << " with the move " << search_stats_->winning_move_.as_string();
    }
    if (depth == 0 && !winning && search_stats_->winning_){
      if(!search_stats_->winning_threads_adjusted){
	search_stats_->winning_ = false;
	if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "The helper engine thinks the root position is no longer winning: cp = " << eval << " and since the autopilot is not yet on, I will not turn it on.";
      }
    }
    // make sure the currently recommended move from the helper is available if it is needed when vetoing Leelas move. TODO change name from "winning" to "recommended".
    if(depth == 0){
      search_stats_->number_of_nodes_in_support_for_helper_eval_of_root = nodes_to_support;
      search_stats_->winning_move_ = my_moves_from_the_white_side.front();
    }
    if(thread == 1){ // assume thread 1 works with leelas preferred child of root.
      search_stats_->number_of_nodes_in_support_for_helper_eval_of_leelas_preferred_child_of_root = nodes_to_support;
    }
    search_stats_->best_move_candidates_mutex.unlock();    
    // Prepare autopilot and blunder vetoing STOP
    
    long unsigned int size;
    search_stats_->fast_track_extend_and_evaluate_queue_mutex_.lock(); // lock this queue before starting to modify it
    size = search_stats_->fast_track_extend_and_evaluate_queue_.size();
    if(size < 20000){ // safety net, silently drop PV:s if we cannot extend nodes fast enough. lc0 stalls when this number is too high.
      search_stats_->fast_track_extend_and_evaluate_queue_.push(my_moves_from_the_white_side);
      search_stats_->starting_depth_of_PVs_.push(depth);
      search_stats_->amount_of_support_for_PVs_.push(nodes_to_support);
      search_stats_->fast_track_extend_and_evaluate_queue_mutex_.unlock();
    } else {
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Thread " << thread << ": Silently discarded a PV starting at depth " << depth << " with " << nodes_to_support  << " nodes to support it. Queue has size: " << size;
      // just unlock
      search_stats_->fast_track_extend_and_evaluate_queue_mutex_.unlock();	
    }
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Thread " << thread << ": Added a PV starting at depth " << depth << " with " << nodes_to_support  << " nodes to support it. Queue has size: " << size;
  } else {
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Ignoring pv because it not of length " << min_pv_size << " or more. Actual size: " << pv_moves.size();
  }
}

void Search::DoAuxEngine(Node* n, int index){
  // before trying to take a lock on nodes_mutex_, always check if search has stopped, in which case we return early
  // if(stop_.load(std::memory_order_acquire)) {
  //   if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine, thread " << index << " caught a stop signal beforing doing anything.";
  //   return;
  // }

  // If we are thread 1:
  // If there is a helper PV
  // 1. then put the node back into the queue,
  // 2. find the divergence between Leela and helper. Record the depth of this divergence so that others can tell if a change in either PV requires me to change node to explore. If the change is deeper, no need to interrupt.
  // 3. explore the node Leela prefers at the divergence infinitely
  // 4. someone else will stop the helper if Leela or helper change their mind.
  // 5. make sure the eval is reported so it can be used to veto leelas move.

  // step 0 make sure that helper has a preferred move
  if(index == 1){
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(300ms); // Let Leela settle on a PV.
    // TODO write code in MaybeTriggerStop that checks if Leelas PV has changed.

    search_stats_->best_move_candidates_mutex.lock();
    std::vector<Move> helper_PV_local = search_stats_->helper_PV;
    search_stats_->best_move_candidates_mutex.unlock();
    if(helper_PV_local.size() > 0){

      // step 1
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Thread 1 in DoAuxEngine() about to try to aquire a lock on auxengine_";
      search_stats_->auxengine_mutex_.unlock();  
      search_stats_->persistent_queue_of_nodes.push(n);
      auxengine_cv_.notify_one();
      search_stats_->auxengine_mutex_.unlock();

      // step 2, find the divergence.
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Thread 1 in DoAuxEngine() about to find the divergence.";
      // First node which does not have an edge that can be found in helper_PV_local is the node to explore
      bool divergence_found = false;
      std::vector<Move> Leelas_PV;
      int depth = 0;
      Node * divergent_node = root_node_;

      nodes_mutex_.lock_shared();
      for(long unsigned int i = 0; i < helper_PV_local.size(); i++){
	Leelas_PV.push_back(GetBestChildNoTemperature(divergent_node, 0).edge()->GetMove());
	LOGFILE << "move " << i << " recommended by leela: " << Leelas_PV[i].as_string();
	LOGFILE << "move " << i << " recommended by helper: " << helper_PV_local[i].as_string();
	divergent_node = GetBestChildNoTemperature(divergent_node, 0).node();	
	if(Leelas_PV[i].as_string() != helper_PV_local[depth].as_string()){
	  LOGFILE << "Found the divergence between helper and Leela at depth: " << i << " node: " << divergent_node->DebugString() << " Leela prefers: " << divergent_node->GetOwnEdge()->GetMove().as_string() << " The helper prefers: " << helper_PV_local[i].as_string();
	  divergence_found = true;
	  depth = i;
	  break;
	}
      }
      nodes_mutex_.unlock_shared();
      
      if(divergence_found){
	n = divergent_node;
	search_stats_->best_move_candidates_mutex.lock();
	search_stats_->Leelas_PV = Leelas_PV;
	search_stats_->PVs_diverge_at_depth = depth;
	search_stats_->best_move_candidates_mutex.unlock();
      } else {
	return;
      }
    }
  }
  
  // Calculate depth.
  int depth = 0;
  if(n != root_node_){
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " DoAuxEngine() trying to aquire a lock on nodes_ to calculate depth.";
    nodes_mutex_.lock_shared();  
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " DoAuxEngine() aquired a lock on nodes_";
    // if(stop_.load(std::memory_order_acquire)) {
    //   if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Thread: " << index << " DoAuxEngine caught a stop signal before starting to calculate depth.";
    //   nodes_mutex_.unlock_shared();
    //   return;
    // }
    for (Node* n2 = n; n2 != root_node_; n2 = n2->GetParent()) {
      depth++;
    }
    nodes_mutex_.unlock_shared();
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " DoAuxEngine() released a lock on nodes_";    
  }

  int AuxEngineTime;

  // if we are thread 0 or thread 1, we don't have to bother with testing for depth
  if(index > 1){
    search_stats_->auxengine_mutex_.lock();
    // Never add nodes to the queue after search has stopped or final purge is run
    if(stop_.load(std::memory_order_acquire) ||
       search_stats_->final_purge_run){
      // just unset pending so that the node can get picked up during next search.
      n->SetAuxEngineMove(0xffff);
      search_stats_->auxengine_mutex_.unlock();
      return;
    }
    
    if (search_stats_->persistent_queue_of_nodes.size() > 0){ // if there is no node in the queue then accept unconditionally.
      if(depth > 1 &&
	 depth > params_.GetAuxEngineMaxDepth()
	 ){
	// Only generate a random sample if these parameters are true, save a few random samples
	if(float(1.0f)/(depth) < distribution(generator)){
	  // This is exactly what SearchWorker::AuxMaybeEnqueueNode() does, but we are in class Search:: now, so that function is not available.
	  // int source = search_stats_->source_of_queued_nodes.front();
	  // search_stats_->source_of_queued_nodes.pop();
	  search_stats_->persistent_queue_of_nodes.push(n);
	  // search_stats_->source_of_queued_nodes.push(source);
	  auxengine_cv_.notify_one(); // unnecessary?
	  search_stats_->auxengine_mutex_.unlock();
	  return;
	}
      }
    }
    
    // while we have this lock, also read the current value of search_stats_->AuxEngineTime, which is needed later
    AuxEngineTime = search_stats_->AuxEngineTime;
  
    search_stats_->auxengine_mutex_.unlock();
  }
  
  // if(depth > 1 &&
  //    depth > params_.GetAuxEngineMaxDepth()){
  //   // if (params_.GetAuxEngineVerbosity() >= 6) LOGFILE << "DoAuxEngine processing a node with high depth: " << " since sample " << sample << " is less than " << float(1.0f)/(depth);
  // }
    
  // if (params_.GetAuxEngineVerbosity() >= 6) LOGFILE << "DoAuxEngine processing a node with depth: " << depth;

  std::string s = "";
  bool flip = played_history_.IsBlackToMove() ^ (depth % 2 == 0);

  // To get the moves in UCI format, we have to construct a board, starting from root and then apply the moves.
  // Traverse up to root, and store the moves in a vector.
  // When we internally use the moves to extend nodes in the search tree, always use move as seen from the white side.
  // Apply the moves in reversed order to get the proper board state from which we can then make moves in legacy format.
  std::vector<lczero::Move> my_moves;
  std::vector<lczero::Move> my_moves_from_the_white_side;  
  
  // if(stop_.load(std::memory_order_acquire)) {
  //   if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Thread: " << index << " DoAuxEngine caught a stop signal while populating my_moves.";
  //   return;
  // }

  if(n != root_node_){
    nodes_mutex_.lock_shared();
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " DoAuxEngine() aquired a lock on nodes_ in order to create the position for the helper.";  
    for (Node* n2 = n; n2 != root_node_; n2 = n2->GetParent()) {
      my_moves.push_back(n2->GetOwnEdge()->GetMove(flip));
      my_moves_from_the_white_side.push_back(n2->GetOwnEdge()->GetMove());
      flip = !flip;
    }
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " DoAuxEngine() releasing a lock on nodes_.";
    nodes_mutex_.unlock_shared();
  }
  
  // Reverse the order
  std::reverse(my_moves.begin(), my_moves.end());
  std::reverse(my_moves_from_the_white_side.begin(), my_moves_from_the_white_side.end());
    
  ChessBoard my_board = played_history_.Last().GetBoard();
  Position my_position = played_history_.Last();

  // modern encoding
  for(auto& move: my_moves) {
    if (my_board.flipped()) move.Mirror();
    my_board.ApplyMove(move);
    my_position = Position(my_position, move);
    if (my_board.flipped()) move.Mirror();
    s = s + move.as_string() + " ";  // only for debugging
    my_board.Mirror();
  }

  if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "add pv=" << s << " from root position: " << GetFen(played_history_.Last());
  s = "position fen " + GetFen(my_position);
  
  // 1. Only start the engines if we can aquire the auxengine_stopped_mutex
  // 2. Only send anything to the engines if we have aquired that mutex

  search_stats_->auxengine_stopped_mutex_.lock();  
  // Before starting, test if stop_ is set
  if (stop_.load(std::memory_order_acquire)) {
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine caught a stop signal 1.";
    search_stats_->auxengine_stopped_mutex_.unlock();
    return;
  }
  *search_stats_->vector_of_opstreams[index] << s << std::endl;
  auto auxengine_start_time = std::chrono::steady_clock::now();
  bool infinite_exploration = false;
  if(
     (index == 0 &&
     (!params_.GetAuxEngineOptionsOnRoot().empty() || search_stats_->winning_)
     ) ||
     (index == 1 && search_stats_->Leelas_preferred_child_node_ != nullptr)
     ){
    infinite_exploration = true;
    if(index == 0){
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Starting infinite query from root node for thread 0 using the opstream at: " << &search_stats_->vector_of_opstreams[index];
    }
    if(index == 1){
      if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Starting infinite query from leelas preferred child of root for thread 1 using the opstream at: " << &search_stats_->vector_of_opstreams[index];      
    }
    *search_stats_->vector_of_opstreams[index] << "go infinite " << std::endl;
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Started infinite query from root node for thread 0 using the opstream at: " << &search_stats_->vector_of_opstreams[index];    
  } else {
    if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Starting time limited query for thread " << index << " using the opstream at: " << &search_stats_->vector_of_opstreams[index];    
    *search_stats_->vector_of_opstreams[index] << "go movetime " << AuxEngineTime << std::endl;
  }
  if(search_stats_->auxengine_stopped_[index]){
    if (params_.GetAuxEngineVerbosity() >= 10) LOGFILE << "Setting auxengine_stopped_ to false for thread " << index;
    search_stats_->auxengine_stopped_[index] = false;    
  }
  search_stats_->auxengine_stopped_mutex_.unlock();

  std::string prev_line;
  std::string my_line;
  std::string line;
  std::string token;
  std::string my_token;  
  bool stopping = false;
  bool second_stopping = false;
  bool third_stopping = false;
  // bool second_stopping_notification = false;
  while(std::getline(*search_stats_->vector_of_ipstreams[index], line)) {
    // if (params_.GetAuxEngineVerbosity() >= 9 &&
    // 	!second_stopping_notification) {
    //   LOGFILE << "thread: " << index << " auxe:" << line;
    // }

    std::istringstream iss(line);
    iss >> token >> std::ws;

    if (token == "bestmove") {
      iss >> token;
      if(token == "info"){
	if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Hit a case of https://github.com/hans-ekbrand/lc0/issues/9";
	// This is a case of https://github.com/hans-ekbrand/lc0/issues/9
	// bestmove:info" indicates something is corrupted in the input stream.
	// issue `stop`, stay in the loop and try another iteration.
	// TODO: If the next iteration also fails, stop and restart the engine.
	search_stats_->auxengine_stopped_mutex_.lock();
	*search_stats_->vector_of_opstreams[index] << "stop" << std::endl;
	search_stats_->auxengine_stopped_mutex_.lock();	
      } else {
	break;
      }
    }
    prev_line = line;

    // Don't send a second stop command
    if (!stopping) {
      stopping = stop_.load(std::memory_order_acquire);
      if (stopping) {
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " caught a stop signal 2.";	
        // (unless someone else already has sent stop) send stop,
	// stay in loop to get best response, otherwise it
        // will disturb the next iteration.
	// only send stop if we are the first to detect that search has stopped.
	search_stats_->auxengine_stopped_mutex_.lock();
	if(!search_stats_->auxengine_stopped_[index]){
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping the A/B helper Start";
	  *search_stats_->vector_of_opstreams[index] << "stop" << std::endl; // stop the A/B helper	  
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping the A/B helper Stop";
	  search_stats_->auxengine_stopped_[index] = true;
	} else {
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "MaybeTriggerStop() must have already sent stop to the engine for instance: " << index;
	}
	search_stats_->auxengine_stopped_mutex_.unlock();
      } else {
	// Since we are not stopping, do the ordinary stuff
	// parse and queue PV:s even before the search is finished, if the depth is high enough (which will be determined by AuxEncode_and_Enqueue().
	// but only use this if this is indefinite exploration, otherwise we just get a lot of junk.	
	if (token == "info" && infinite_exploration) {
	  AuxEncode_and_Enqueue(line, depth, my_board, my_position, my_moves_from_the_white_side, true, index);
	}
      }
    } else {
      // Stopping is true, but did it happen before or after the helper sent its info line? Assume it was after, in which case the helper is all good.
      if(second_stopping){
	// inspecting the output from the helpers, suggest that it is harmless, just a normal info pv line.
	// perhaps they output at least one such line, and bestmove will come next?
	search_stats_->auxengine_stopped_mutex_.lock();
	*search_stats_->vector_of_opstreams[index] << "stop" << std::endl; // stop the A/B helper
	search_stats_->auxengine_stopped_mutex_.unlock();
	// log statment below turned out to always show perfectly normal output.
	// if (third_stopping && params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "thread: " << index << " We found that search was stopped on the previous iteration, but the current line from the helper was not 'bestmove'. Probably the helper engine does not repond to stop until it has search for some minimum amount of time (like 10 ms). As a workaround send yet another stop. This is the output from the helper: " << line;
	if(!third_stopping){
	  third_stopping = true;
	}
      } else {
        second_stopping = true;
      }
    }
  }
  // if (stopping) {
  //   // Don't use results of a search that was stopped.
  //   // Not because the are unreliable, but simply because we want to shut down as fast as possible.
  //   return;
  // }
  if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "About to aquire the auxengine_stopped_mutex";
  search_stats_->auxengine_stopped_mutex_.lock();
  search_stats_->auxengine_stopped_[index] = true; // stopped means "not running". It does not mean it was stopped prematurely.
  search_stats_->auxengine_stopped_mutex_.unlock();
  
  if (params_.GetAuxEngineVerbosity() >= 9) {
    LOGFILE << "pv:" << prev_line;
    LOGFILE << "bestanswer:" << token;
  }
  if(prev_line == ""){
    if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Thread: " << index << " Empty PV, returning early from doAuxEngine().";
    // // TODO restart the helper engine?
    // using namespace std::chrono_literals;
    // std::this_thread::sleep_for(100ms);
    return;
  }
  if (! search_stats_->vector_of_children[index]->running()) {
    LOGFILE << "AuxEngine died!";
    throw Exception("AuxEngine died!");
  }
  auto auxengine_dur =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - auxengine_start_time)
      .count();
  auxengine_total_dur += auxengine_dur;
  auxengine_num_evals++;
  AuxEncode_and_Enqueue(prev_line, depth, my_board, my_position, my_moves_from_the_white_side, false, index);
  if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Thread: " << index << " Finished at DoAuxEngine().";
}

void Search::AuxWait() {
  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "In AuxWait()";
  while (!auxengine_threads_.empty()) {
    Mutex::Lock lock(threads_mutex_);
    auxengine_threads_.back().join();
    auxengine_threads_.pop_back();
  }
  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWait finished shutting down AuxEngineWorker() threads.";

  // Clear the PV cache.
  search_stats_->my_pv_cache_mutex_.lock();
  int pv_cache_size = search_stats_->my_pv_cache_.size();
  search_stats_->my_pv_cache_.clear();
  search_stats_->my_pv_cache_mutex_.unlock();  

  search_stats_->auxengine_mutex_.lock();

  // Store the size of the queue, for possible adjustment of threshold and time
  search_stats_->AuxEngineQueueSizeAtMoveSelectionTime = search_stats_->persistent_queue_of_nodes.size();
  search_stats_->Total_number_of_nodes = root_node_->GetN() - search_stats_->Total_number_of_nodes;
  if(params_.GetAuxEngineVerbosity() >= 4) LOGFILE << search_stats_->AuxEngineQueueSizeAtMoveSelectionTime << " nodes left in the query queue at move selection time. Threshold used: " << search_stats_->AuxEngineThreshold;

  // purge obsolete nodes in the helper queues. Note that depending on the move of the opponent more nodes can become obsolete.
  if(search_stats_->persistent_queue_of_nodes.size() > 0){
    std::queue<Node*> persistent_queue_of_nodes_temp;
    // std::queue<int> source_of_queued_nodes_temp;
    long unsigned int my_size = search_stats_->persistent_queue_of_nodes.size();
    for(long unsigned int i=0; i < my_size; i++){
      Node * n = search_stats_->persistent_queue_of_nodes.front(); // read the element
      search_stats_->persistent_queue_of_nodes.pop(); // remove it from the queue.
      // int source = search_stats_->source_of_queued_nodes.front(); // read the element
      // search_stats_->source_of_queued_nodes.pop(); // remove it from the queue.
      for (Node* n2 = n; n2 != root_node_ ; n2 = n2->GetParent()) {
	// if purge at search start never happened (because of only one move possible, auxworker() never started), then we can have disconnected nodes in the queue.
	// if(n2->GetParent() == nullptr || n2->GetParent()->GetParent() == nullptr) break;
	if(n2->GetParent() == nullptr || n2->GetParent()->GetParent() == nullptr || n2->GetParent()->GetOwnEdge() == nullptr) break;
	if(n2->GetParent()->GetParent() == root_node_){
	  if(n2->GetParent()->GetOwnEdge()->GetMove(played_history_.IsBlackToMove()) == final_bestmove_){
	    persistent_queue_of_nodes_temp.push(n);
	    // in order to be able to purge nodes that became obsolete and deallocated due to the move of the opponent,
	    // also save the grandparent that will become root at next iteration if this node is still relevant by then.
	    persistent_queue_of_nodes_temp.push(n2);
	    // source_of_queued_nodes_temp.push(source);
	  }
	  break;
	}
      }
    }
    long unsigned int size_kept = persistent_queue_of_nodes_temp.size() / 2;
    for(long unsigned int i=0; i < size_kept * 2; i++){
      search_stats_->persistent_queue_of_nodes.push(persistent_queue_of_nodes_temp.front());
      persistent_queue_of_nodes_temp.pop();
    }
      
    if(params_.GetAuxEngineVerbosity() >= 4)
      LOGFILE << "Purged " << my_size - size_kept
	      << " nodes in the query queue based the selected move: " << final_bestmove_.as_string()
	      << ". " << size_kept << " nodes remain. Sanity check size is " << search_stats_->persistent_queue_of_nodes.size();
    search_stats_->AuxEngineQueueSizeAfterPurging = size_kept;
  } else {
    if(params_.GetAuxEngineVerbosity() >= 4)      
      LOGFILE << "No nodes in the query queue at move selection";
  }

  // search_stats_->final_purge_run = true; // Inform Search::AuxEngineWorker(), which can start *AFTER* us, that we have already purged stuff. If they also do it, things will break badly.
  
  search_stats_->Number_of_nodes_added_by_AuxEngine = search_stats_->Number_of_nodes_added_by_AuxEngine + auxengine_num_updates;
  float observed_ratio = float(search_stats_->Number_of_nodes_added_by_AuxEngine) / search_stats_->Total_number_of_nodes;

  // // Decrease the EngineTime if we're in an endgame.
  // ChessBoard my_board = played_history_.Last().GetBoard();
  // if((my_board.ours() | my_board.theirs()).count() < 20){
  //   search_stats_->AuxEngineTime = std::max(10, int(std::round(params_.GetAuxEngineTime() * 0.50f))); // minimum 10 ms.
  // }

  // Time based queries    
  if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Summaries per move: (Time based queries) persistent_queue_of_nodes size at the end of search: " << search_stats_->AuxEngineQueueSizeAtMoveSelectionTime
	  << " Ratio added/total nodes: " << observed_ratio << " (added=" << search_stats_->Number_of_nodes_added_by_AuxEngine << "; total=" << search_stats_->Total_number_of_nodes << ")."
      << " Average duration " << (auxengine_num_evals ? (auxengine_total_dur / auxengine_num_evals) : -1.0f) << "ms"
      << " AuxEngineTime for next iteration " << search_stats_->AuxEngineTime
      << " New AuxEngineThreshold for next iteration " << search_stats_->AuxEngineThreshold
      << " Number of evals " << auxengine_num_evals
      << " Number of added nodes " << search_stats_->Number_of_nodes_added_by_AuxEngine
      << " Entries in the PV cache: " << pv_cache_size
      << " Called AuxMaybeEnqueueNode() " << number_of_times_called_AuxMaybeEnqueueNode_ << " times.";

  // Reset counters for the next move:
  search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
  search_stats_->Total_number_of_nodes = 0;
  search_stats_->auxengine_mutex_.unlock();

  search_stats_->Leelas_preferred_child_node_ = nullptr;

  // initial_purge_run needs another lock.
  search_stats_->pure_stats_mutex_.lock();
  search_stats_->initial_purge_run = false;
  search_stats_->pure_stats_mutex_.unlock();
  // Empty the other queue.
  search_stats_->fast_track_extend_and_evaluate_queue_mutex_.lock();
  if(search_stats_->fast_track_extend_and_evaluate_queue_.empty()){
    if (params_.GetAuxEngineVerbosity() >= 4) LOGFILE << "No PVs in the fast_track_extend_and_evaluate_queue";
  } else {
    if (params_.GetAuxEngineVerbosity() >= 4) LOGFILE << search_stats_->fast_track_extend_and_evaluate_queue_.size() << " possibly obsolete PV:s in the queue, checking which of them are still relevant based on our move " << final_bestmove_.as_string();

    // Check if the first move in each PV is the move we played
    // Store the PVs that are still relevant in a temporary queue
    std::queue<std::vector<Move>> fast_track_extend_and_evaluate_queue_temp_;
    long unsigned int my_size = search_stats_->fast_track_extend_and_evaluate_queue_.size();
    for(long unsigned int i=0; i < my_size; i++){
      std::vector<Move> pv = search_stats_->fast_track_extend_and_evaluate_queue_.front();
      search_stats_->fast_track_extend_and_evaluate_queue_.pop();
      // final_bestmove_ is not necessarily from white's point of view.
      // but pv[0] is always from white's point of view.
      Move m;
      Move::ParseMove(&m, pv[0].as_string(), played_history_.IsBlackToMove());
      // m is now rotated if needed
      if(m == final_bestmove_){
	// remove the first move, which is the move we just played
	pv.erase(pv.begin());
	fast_track_extend_and_evaluate_queue_temp_.push(pv);
      }
    }
    // Empty the queue and copy back the relevant ones.
    search_stats_->fast_track_extend_and_evaluate_queue_ = {};
    long unsigned int size_kept = fast_track_extend_and_evaluate_queue_temp_.size();
    for(long unsigned int i=0; i < size_kept; i++){
      search_stats_->fast_track_extend_and_evaluate_queue_.push(fast_track_extend_and_evaluate_queue_temp_.front());
      fast_track_extend_and_evaluate_queue_temp_.pop();
    }
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Number of PV:s in the queue after purging: " << search_stats_->fast_track_extend_and_evaluate_queue_.size();
  }
  search_stats_->fast_track_extend_and_evaluate_queue_mutex_.unlock();
  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWait done search_stats_ at: " << &search_stats_;
}

}  // namespace lczero
