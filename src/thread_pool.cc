#include "casts.h"
#include "runtime.h"
#include "stl_util.h"
#include "thread.h"
#include "thread_pool.h"

namespace art {

ThreadPoolWorker::ThreadPoolWorker(ThreadPool* thread_pool, const std::string& name,
                                   size_t stack_size)
    : thread_pool_(thread_pool),
      name_(name),
      stack_size_(stack_size) {
  const char* reason = "new thread pool worker thread";
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), reason);
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), reason);
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, &attr, &Callback, this), reason);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), reason);
}

ThreadPoolWorker::~ThreadPoolWorker() {
  CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "thread pool worker shutdown");
}

void ThreadPoolWorker::Run() {
  Thread* self = Thread::Current();
  Task* task = NULL;
  while ((task = thread_pool_->GetTask(self)) != NULL) {
    task->Run(self);
    task->Finalize();
  }
}

void* ThreadPoolWorker::Callback(void* arg) {
  ThreadPoolWorker* worker = reinterpret_cast<ThreadPoolWorker*>(arg);
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->AttachCurrentThread(worker->name_.c_str(), true, NULL));
  // Do work until its time to shut down.
  worker->Run();
  runtime->DetachCurrentThread();
  return NULL;
}

void ThreadPool::AddTask(Thread* self, Task* task){
  MutexLock mu(self, task_queue_lock_);
  tasks_.push_back(task);
  // If we have any waiters, signal one.
  if (waiting_count_ != 0) {
    task_queue_condition_.Signal(self);
  }
}

ThreadPool::ThreadPool(size_t num_threads)
  : task_queue_lock_("task queue lock"),
    task_queue_condition_("task queue condition", task_queue_lock_),
    completion_condition_("task completion condition", task_queue_lock_),
    started_(false),
    shutting_down_(false),
    waiting_count_(0) {
  while (GetThreadCount() < num_threads) {
    const std::string name = StringPrintf("Thread pool worker %zu", GetThreadCount());
    threads_.push_back(new ThreadPoolWorker(this, name, ThreadPoolWorker::kDefaultStackSize));
  }
}

ThreadPool::~ThreadPool() {
  {
    Thread* self = Thread::Current();
    MutexLock mu(self, task_queue_lock_);
    // Tell any remaining workers to shut down.
    shutting_down_ = true;
    // Broadcast to everyone waiting.
    task_queue_condition_.Broadcast(self);
    completion_condition_.Broadcast(self);
  }
  // Wait for the threads to finish.
  STLDeleteElements(&threads_);
}

void ThreadPool::StartWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = true;
  task_queue_condition_.Broadcast(self);
  start_time_ = NanoTime();
  total_wait_time_ = 0;
}

void ThreadPool::StopWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = false;
}

Task* ThreadPool::GetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  while (!IsShuttingDown()) {
    Task* task = TryGetTaskLocked(self);
    if (task != NULL) {
      return task;
    }

    waiting_count_++;
    if (waiting_count_ == GetThreadCount() && tasks_.empty()) {
      // We may be done, lets broadcast to the completion condition.
      completion_condition_.Broadcast(self);
    }
    const uint64_t wait_start = NanoTime();
    task_queue_condition_.Wait(self);
    const uint64_t wait_end = NanoTime();
    total_wait_time_ += wait_end - std::max(wait_start, start_time_);
    waiting_count_--;
  }

  // We are shutting down, return NULL to tell the worker thread to stop looping.
  return NULL;
}

Task* ThreadPool::TryGetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  return TryGetTaskLocked(self);
}

Task* ThreadPool::TryGetTaskLocked(Thread* self) {
  if (started_ && !tasks_.empty()) {
    Task* task = tasks_.front();
    tasks_.pop_front();
    return task;
  }
  return NULL;
}

void ThreadPool::Wait(Thread* self, bool do_work) {
  Task* task = NULL;
  while ((task = TryGetTask(self)) != NULL) {
    task->Run(self);
    task->Finalize();
  }

  // Wait until each thread is waiting and the task list is empty.
  MutexLock mu(self, task_queue_lock_);
  while (!shutting_down_ && (waiting_count_ != GetThreadCount() || !tasks_.empty())) {
    completion_condition_.Wait(self);
  }
}

size_t ThreadPool::GetTaskCount(Thread* self){
  MutexLock mu(self, task_queue_lock_);
  return tasks_.size();
}

WorkStealingWorker::WorkStealingWorker(ThreadPool* thread_pool, const std::string& name,
                                       size_t stack_size)
    : ThreadPoolWorker(thread_pool, name, stack_size),
      task_(NULL) {

}

void WorkStealingWorker::Run() {
  Thread* self = Thread::Current();
  Task* task = NULL;
  WorkStealingThreadPool* thread_pool = down_cast<WorkStealingThreadPool*>(thread_pool_);
  while ((task = thread_pool_->GetTask(self)) != NULL) {
    WorkStealingTask* stealing_task = down_cast<WorkStealingTask*>(task);

    {
      CHECK(task_ == NULL);
      MutexLock mu(self, thread_pool->work_steal_lock_);
      // Register that we are running the task
      ++stealing_task->ref_count_;
      task_ = stealing_task;
    }
    stealing_task->Run(self);
    // Mark ourselves as not running a task so that nobody tries to steal from us.
    // There is a race condition that someone starts stealing from us at this point. This is okay
    // due to the reference counting.
    task_ = NULL;

    bool finalize;

    // Steal work from tasks until there is none left to steal. Note: There is a race, but
    // all that happens when the race occurs is that we steal some work instead of processing a
    // task from the queue.
    while (thread_pool->GetTaskCount(self) == 0) {
      WorkStealingTask* steal_from_task  = NULL;

      {
        MutexLock mu(self, thread_pool->work_steal_lock_);
        // Try finding a task to steal from.
        steal_from_task = thread_pool->FindTaskToStealFrom(self);
        if (steal_from_task != NULL) {
          CHECK_NE(stealing_task, steal_from_task)
              << "Attempting to steal from completed self task";
          steal_from_task->ref_count_++;
        } else {
          break;
        }
      }

      if (steal_from_task != NULL) {
        // Task which completed earlier is going to steal some work.
        stealing_task->StealFrom(self, steal_from_task);

        {
          // We are done stealing from the task, lets decrement its reference count.
          MutexLock mu(self, thread_pool->work_steal_lock_);
          finalize = !--steal_from_task->ref_count_;
        }

        if (finalize) {
          steal_from_task->Finalize();
        }
      }
    }

    {
      MutexLock mu(self, thread_pool->work_steal_lock_);
      // If nobody is still referencing task_ we can finalize it.
      finalize = !--stealing_task->ref_count_;
    }

    if (finalize) {
      stealing_task->Finalize();
    }
  }
}

WorkStealingWorker::~WorkStealingWorker() {

}

WorkStealingThreadPool::WorkStealingThreadPool(size_t num_threads)
    : ThreadPool(0),
      work_steal_lock_("work stealing lock"),
      steal_index_(0) {
  while (GetThreadCount() < num_threads) {
    const std::string name = StringPrintf("Work stealing worker %zu", GetThreadCount());
    threads_.push_back(new WorkStealingWorker(this, name, ThreadPoolWorker::kDefaultStackSize));
  }
}

WorkStealingTask* WorkStealingThreadPool::FindTaskToStealFrom(Thread* self) {
  const size_t thread_count = GetThreadCount();
  for (size_t i = 0; i < thread_count; ++i) {
    // TODO: Use CAS instead of lock.
    ++steal_index_;
    if (steal_index_ >= thread_count) {
      steal_index_-= thread_count;
    }

    WorkStealingWorker* worker = down_cast<WorkStealingWorker*>(threads_[steal_index_]);
    WorkStealingTask* task = worker->task_;
    if (task) {
      // Not null, we can probably steal from this worker.
      return task;
    }
  }
  // Couldn't find something to steal.
  return NULL;
}

WorkStealingThreadPool::~WorkStealingThreadPool() {

}

}  // namespace art
