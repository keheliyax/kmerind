/**
 * @file    CommunicationLayer.hpp
 * @ingroup bliss::io
 * @author  Patrick Flick <patrick.flick@gmail.com>
 * @author  Tony Pan <tpan@gatech.edu>
 * @brief   contains a class that abstracts MPI point-to-point, non-blocking messaging.
 *
 * Copyright (c) TODO
 *
 * TODO add Licence
 */

#ifndef BLISS_COMMUNICATION_LAYER_HPP
#define BLISS_COMMUNICATION_LAYER_HPP

#include <mpi.h>
#include <omp.h>

// C stdlib includes
#include <assert.h>
#include <cstdio>  // fflush

// STL includes
#include <queue>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <functional>
#include <utility>    // std::move

// BLISS includes
#include "utils/logging.h"
#include "concurrent/threadsafe_queue.hpp"
#include "concurrent/copyable_atomic.hpp"
#include "concurrent/concurrent.hpp"
#include "io/locking_message_buffers.hpp"
#include "io/io_exception.hpp"
#include "io/message_type_info.hpp"
#include "io/message_types.hpp"
#include "io/mpi_comm_thread.hpp"

namespace bliss
{

namespace io
{


/**
 * @brief Abstracts asynchronous and buffered point to point communication between all processes via MPI.
 * @details   This class encapsulates the MPI message handling for point to point asynchronous messaging
 *        for a MPI communicator.  It supports multiple concurrent message producers and consumers
 *
 *        The following requirements are satisfied:
 *        1. support application where there are different message types (e.g. query-response).
 *        2. support collectively, blocking flushing all messages of a particular type.
 *        3. provide flexible way of handling received messages based on message type
 *        4. Thread Safe, non-blocking send (allow overlap of computation with communication)
 *        5. Thread Safe processing of received messages
 *        6. single threaded control of the Communication Layer (flush messages, finish, etc)
 *
 *        Design and Implementation  (MORE DETAILS BELOW)
 *        1. support application where there are different message types (e.g. query-response).
 *          each data message type is assigned an id ranging from 1 to MAX_INT.
 *          FOC messages have tag 0, but carries in payload the tag of the message type that is being controlled.
 *          in addition, FOC messages have an additional, sequentially assigned id for each message type to
 *          disambiguate multiple FOC messages with the same tag
 *
 *        2. support collective, blocking flushing of all messages of a particular type.
 *          To indicate to the system that there is no further messages of a particular type, collective flush
 *          operations have been implemented.  There are 3 types:
 *          a. "flush":  flush a single message type, allow future messages of this type
 *          b. "finish": flush a single message type, disallow future messages of this type
 *          c. "finish communication":  flush all message types, and mark end of application communication)
 *
 *          As these are collective functions, all MPI processes need to make matching calls.
 *          The timing of the calls need not be synchronous, but they need to preserve the order of the calls
 *
 *          The functions form barriers, and blocks the calling thread
 *          until all processes have completely processed all matching FOC messages from all senders
 *
 *        3. provide flexible way of handling received messages based on message type
 *          Callbacks can be registered, one per message type.  When processing received messages,
 *          the matching callback is used.
 *
 *        4. Thread Safe, non-blocking send (allow overlap of computation with communication)
 *          A compute thread can call sendMessage on the Communication Layer object.  each invocation
 *          atomically gets a location to insert the message into a batching buffer via thread safe insert/
 *
 *          When a buffer is full and sent, it is inserted into a thread-safe send queue for the sending comm thread to
 *          send via MPI  (non-blocking).
 *
 *        5. Thread Safe, non-blocking receive
 *          A dedicated thread checks for MPI messages to receive remote messages and populate the recvQueue. Separation
 *          of sender and receiver communication threads avoids deadlock when the receiving process is blocked by a full
 *          recvQueue, thus blocking the sending function.
 *
 *
 *        6. Thread Safe processing of received messages
 *          Multiple callback threads can process the received messages in the receive queue, which is thread safe.
 *
 *        6. single threaded control of the Communication Layer (flush messages, finish, etc)
 *          flush and finish functions are collectively called by the control thread (owner of Communication Layer object)
 *
 *
 *        Basic Concepts and Design:
 *        A. Threading
 *          The following are the intended threading pattern:
 *          * single control thread (owner of Communication Layer instance.  can be one of the compute threads)
 *          * one or more compute threads (producer of messages to be sent)
 *          * single MPI communication thread for send (internal)
 *          * single MPI communication thread for recv (internal)
 *          * one or more receive threads (consumers of messages received, internal)
 *
 *        The choice of using a single MPI communication thread is based on
 *          1. Single threaded MPI libraries are more readily available on different platforms.
 *          2. Single threaded MPI communication enforces message ordering (especially when sending control messages)
 *          3. Single threaded MPI communication reduce opportunities for deadlock due to thread timing.
 *
 *        The single MPI comm thread and multiple producer/consumer pattern requires thread-safe queues as a mechanism
 *          for inter-thread communication.
 *
 *        To avoid race condition amongst multiple compute threads or receive threads:
 *          1. Compute threads insert messages into Buffers (bliss::io::Buffer), which is thread safe for insertion by using CAS operations to get position for insert.
 *          2. When a buffer is full, compute thread enqueues it for MPI send, and a empty buffer is swapped in from BufferPool.  BufferPool is also thread safe.
 *          3. When multiple receive threads dequeue to process received messages buffers, they do so from a thread safe queue.
 *
 *
 *        B. Data Types
 *          Message: a message has a payload and a type id.
 *
 *          There are 2 families of messages:  data and FOC messages.
 *          Data Messages have actual data as its payload, and the message type id as its MPI tag.
 *          FOC messages have 2 integers as its payload: type id of the message to be controlled, and an epoch id
 *            FOC messages have 0 as its message type.
 *
 *          For why epoch id is needed, see "Rationales"
 *
 *        C. MPI Messaging
 *          Data messages are transmitted using MPI non-blocking Point to Point communication: isend, iprobe + irecv
 *            due to the non-blocking calls, also use queue to manage send and receive in progress.
 *          FOC messages are transmitted using MPI bsend to simplify memory management.
 *          Local messages (where src and destination ranks are the same) are sent via memory (direct insert into receive queue)
 *
 *          Collective operations (flush, finish; for requirement 2) are implemented using Point to Point communication of FOC messages.
 *          The messaging pattern is peer-to-peer (no master-slave relationship).
 *            as a consequence, collective operations require all to send to all (in a non-blocking way)
 *          not using MPI AllToAllv because that blocks the compute threads
 *            TODO: perhaps use master-slave, with rotating master based on epoch id.  reduces comm from p^2 for each epoch to 2p.
 *
 *          flush requires:
 *            1. called collectively by all control threads
 *            2. blocks until the process has received AND PROCESSED the FOC message from ALL senders. (to ensure collectiveness)
 *                a. As message ordering is strict, all messages prior to the FOC messages, including all data messages of the specified
 *                  type, are required to be processed as well.
 *          Flush and finish therefore serve as synchronization points in the application logic, and can also be viewed as MPI messaging fences.
 *          The synchronization point defines EPOCHs, which are periods between synchronization calls.  each epoch is assigned an id.
 *
 *          Typical flow of control for sending data is:
 *            1. compute thread adds message to CommunicationLayer, which is added to a buffer.
 *                buffer is enqueued in send-queue when full.)
 *            2. MPI thread pops from send queue and initiates MPI isend
 *              a. isend requests are queued in 'sendInProgress' queue, and checked for completion.
 *            3. MPI thread probes for data.  if found,
 *              a. buffer allocated and irecv initiated, with request pushed into recvInProgress queue.
 *              b. on completion of receive, data is moved to receive queue.
 *            4. receive threads pops from receive queue and processed
 *
 *          Typical flow of control for flushing:
 *            1. control thread calls flush or finish, which enqueues FOC messages, then blocks
 *            2. MPI thread pops from queue and initiates blocking buffered send (bsend)
 *            3. MPI thread probes for data.  if FOC message found,
 *              a. buffer allocated and irecv initiated, with request pushed into recvInProgress queue.
 *              b. on completion of receive, decrement the FOC message count for the specified epoch.
 *                when 0 (received from all senders), an END message is inserted into receive queue.
 *            4. when FOC messages are popped from receive queue, receive thread unblocks control thread's flush call.
 *
 *          Rationale for NOT using MPI collective functions: see Rationales below.
 *
 *          For more detail, see Rationale and Design and Implementation point 2 below
 *
 *        D. Termination signal propagation and comm/receive thread termination
 *          Since we have multiple processes, and multiple threads queuing message for sent, receiving, and processing received messages,
 *          it is important to define how termination signals are propagated through the application.
 *
 *          The CommThread loop terminates when both sending and receiving are complete.
 *            a. sending is completed when finishCommunication has been called and all pending messages are sent,
 *               and all in-progress send requests are completed.
 *            b. receiving is completed when no Application Termination FOC messages (tag == CONTROL_TAG, the last control messages)
 *                are left to receive.  At init, we set the number of Application Termination FOC messages
 *                to the communication size, which are only decremented after finishCommunication is called.
 *
 *          The Receive Thread terminates when receiving is completed and there are no pending received messages to process
 *            a. receiving is completed according to criteria above (b in previous paragraph).
 *                A single Application Termination FOC message would then be queued for processing.
 *            b. Queue for received messages that are pending processing is exhausted.
 *
 *
 *        Rationales
 *          1. Why do we need epoch Ids?
 *              Epoch ids allows the receiver to group all FOC messages with matching tag and epoch from different sources
 *              together.  It also ensure that a receiver gets EXACTLY ONE FOC message from each source during a epoch.
 *              Thus it helps to separate and preserve order of collective calls.
 *
 *              This is particularly important when different MPI processses may be sending FOC messages at different rates.
 *              For example, without epoch id, and using number of FOC messages received == number of senders as flush unblocking
 *              criteria, we would encounter the following problem:  Process A, B, and C generate 0, 1, and 2
 *              flushes for tag X.  B and C, listening for FOC messages with tag X, will receive 3 FOC messages
 *              with tag X, thus unblocked from flush, but they never received messages from A. A, when it finally enters flush, will
 *              have 4 FOC messages with tag X.  If count of FOC message received were reset at this point, subsequent Flush could
 *              be waiting for a FOC message that has already been processed in the last Flush, resulting in deadlock.
 *
 *              An INEFFICIENT alternative is to count the FOC messages per tag from each sender, but then each process
 *              would need O(m*p) memory (m = number of tags, p = number of processes) and O(p^2) time (p FOC messages,
 *              each FOC message with tag X received results in checking an array of size p for possible completion of a flush)
 *
 *              The final design is to use epoch.  For each epoch id and tag id, we count down the number of FOC messages
 *              with that epoch and tag, until the count reaches 0, at which time the flush for that epoch completes.
 *
 *              While this ensures collectiveness of flush call, it does not allow MPI processes to send FOC messages out of order.
 *              If process A flushes tag 1 then tag 2, and process B flushes Tag 2 then Tag 1, because they each locally enter
 *              a blocking wait for the flush operation to complete, the application will deadlock.
 *              example:
 *                proc 1              proc 2
 *                  tag   1 2 1 2         tag   1 2 2 1
 *                  epoch 0 1 2 3         epoch 0 1 2 3
 *              Note that this is NOT different than standard MPI behavior for point-to-point or collective operations.
 *
 *
 *          2. why not use MPI collectives?
 *            1. collective functions called in MPI comm thread needs to occur AFTER
 *                comm thread have cleared send queue, all pending send and receive requests,
 *                and receive threads have processed all received data messages - inter-thread coordination required.
 *            2. a slow process may post send AFTER a fast process already entered collective function call,
 *                resulting in data messages received out of order, likely creating deadlock (as slow process
 *                is waiting for receive before calling collective function) - inter-process coordination required.
 *            The inter-process coordination would require some marker to indicate "last message"
 *              prior synchronization, which is the exact design we have above, without needing MPI collective functions.
 *            In addition, by using Point-To-Point communication, the FOC messages are enqueued like other messages,
 *              so no explicit "wait until queue cleared" is required, thus reducing idle time.
 *
 *        Usage
 *          Control thread instantiates CommunicationLayer with MPI communicator
 *          Control thread addReceiveCallbacks to CommLayer
 *          Control thread calls initCommunication()
 *
 *          in parallel
 *            compute threads call sendMessage(...)
 *
 *          Control thread call flush(tag)
 *
 *          in parallel
 *            compute threads call sendMessage(...)
 *
 *          Control thread call flush(tag)
 *
 *          ...
 *          (internally CommLayer's receive threads call the callbacks.)
 *          ...
 *
 *          Control thread call finish(tag)
 *
 *          control thread call finishCommunication()
 *
 *          Control thread call finalize (TO MAKE SURE ALL THREADS ARE FINISHED)
 *
 * TODO - replace uint8_t by typedef
 *
 */
class CommunicationLayer
{


protected:
   /// alias MessageBuffersType to Use the thread safe version of the SendMessageBuffers
   using MessageBuffersType = SendMessageBuffers<bliss::concurrent::LockType::SPINLOCK, bliss::concurrent::LockType::LOCKFREE, 8192>;

   /// alias BufferPoolType to MessageBuffersType's
   using BufferPoolType = typename MessageBuffersType::BufferPoolType;

   /// alias BufferPtrType to MessageBuffersType's
   using BufferPtrType = typename MessageBuffersType::BufferType*;

   using MessageTypeInfo = typename bliss::io::MessageTypeInfo<MessageBuffersType>;

    //==== Message type aliases for convenience.




    // callback thread,  for organization.
    // TODO: group the code for callback thread.
    class CallbackThread
    {

      public:
        CallbackThread(CommunicationLayer& _comm_layer) :
          commLayer(_comm_layer), callbackFunctions(),
          td() {

        };
        CallbackThread() = delete;

        virtual ~CallbackThread() {
          callbackFunctions.clear();
        }

        void start() {
          if (!td.joinable()) td = std::move(std::thread(&bliss::io::CommunicationLayer::CallbackThread::run, this));
        }

        void finish() {
          if (td.joinable()) td.join();
        }

        /**
         * @brief Registers a callback function for the given message tag.
         * @details
         *      The registered callback function will be called for each received message
         *      of the given type (tag). A callback function has to be registered for each
         *      message type sent. Only one callback function can be registered for each
         *      tag, adding more than one results in a `invalid_argument` exception.
         *
         *      A callback function has the signature:
         *         void(uint8_t* msg, std::size_t count, int fromRank)
         *
         * @note  It is STRONGLY ENCOURAGED that the function be THREAD SAFE, as there may be
         *      more than 1 receive thread calling the functions.
         *
         * @param tag                 The message tag for which the callback function
         *                            is registered. Has to be > 0.
         * @param callbackFunction    The callback-function.
         *
         * @throw std::invalid_argument   If `tag <= 0` or if a callback function has
         *                                already been registered for the given tag.
         */
        void addReceiveCallback(int tag, std::function<void(uint8_t*, std::size_t, int)> callbackFunction) throw (std::invalid_argument)
        {

          // check for valid arguments
          assert(tag != CONTROL_TAG && tag >= 0);

          // one thread calls this at a time.
          std::unique_lock<std::recursive_mutex> lock(commLayer.mutex);

          if (callbackFunctions.count(tag) > 0) {
            lock.unlock();
            throw std::invalid_argument("M callback function already registered for given tag");
          }

          // add the callback function to a lookup table
          callbackFunctions[tag] = callbackFunction;
          lock.unlock();
        }

      protected:
        /// parent (containing) class for this thread.
        CommunicationLayer& commLayer;

        /// Registry of callback functions, mapped to by the associated tags
        std::unordered_map<int, std::function<void(uint8_t*, std::size_t, int)> > callbackFunctions;

        /// std::thread object for the dedicated callback-handler thread
        std::thread td;

        /**
         * @brief The function for the dedicated callback-handler thread.
         * @details   loops until all received messages in queue are processed, and no more possibility of receiving messages.
         *
         *        Control messages are added in the queue if receiver has received that message from all senders, so it marks the
         *        completion of flush or finish or finishCommunication, which are unblocked.
         *
         *        Data messages are processed directly via callbacks according to the message type (in tag).
         *
         * @note  can have multiple instances of this.
         */
        void run()
        {
          DEBUGF("B THREAD STARTED:  callback-thread on %d", commLayer.commRank);


          // TODO: use thread lock mutex and cond var to help reduce load.


          // while there are messages to process, or we are still receiving
          while (commLayer.recvQueue.canPop() )  // same as !recvDone || !recvQueue.isEmpty()
          {
            // get next element from the queue, wait if none is available.
            // waitAndPop will exit out of wait when termination flag is set on the recvQueue
            auto result = std::move(commLayer.recvQueue.waitAndPop());
            if (!result.first) {
              // no valid result, we must be done with receiving
              assert(!commLayer.recvQueue.canPush());
              break;
            }

            // get the message (data or control)

            // if the message is a control message (enqueued ONE into RecvQueue only after all control messages
            //  for a tag have been received.)
            if (result.second.get()->tag == CONTROL_TAG) {   // Control message

              ControlMessage* msg = dynamic_cast<ControlMessage*>(result.second.get());

              // get control message's target tag.
              TaggedEpoch te = msg->control;


              //===== and unblock the flush/finish/finishCommunication call.
              // getting to here means that all control messages for a tag have been received from all senders.
              // this means the local "sendControlMessages" has been called.  now, look at order of calls in waitForControlMessage
              //
              // main thread:  lock; flushing = notag->tag; flushing==tag?; cv_wait;
              // callback thread:  lock; flushing = tag->notag ? cv_notify_all : return message to front of queue
              // to ensure the flushing is modified by 1 thread at a time, and notify_all does not get interleaved to before wait,
              // lock is used to enclose the entire sequence on each thread.
              // also if callback thread's block is called before the main thread, we don't want to discard the control message - requeue.
              commLayer.ctrlMsgProperties.at(MessageTypeInfo::getTag(te)).lock();

              // if flushing is not tag, then some other tag is being processed, or flushing == notag.
              // if notag - this block is called before waitForControlMessages.  need to requeue
              // if some other tag (tag1), that means we received all tag2 messages in comm thread before receiving all tag1 messages
              // therefore recv thread is processing tag2 before tag1 control message.
              // if the delayed tag1 message is from remote process, and assume that process called sendControlMessages tag1 before tag2,
              //  then strict ordering of MPI messages is violated.
              // if the delayed tag1 message is from local process, because in mem message movement, tag1 is always before tag2.
              // so tag2 cannot come before tag1, provided that all MPI processes call sendControlMessage and waitForControlMessages in the same order
              //  including requirement that callers of sendControlMessage and waitForControlMessages reside in 1 thread only.

              // to deal with multiple threads that can use control messages, we'd need to have an array of "flushing" variables, array of mutices (sharing would render the threads serial),
              // and array of lock and condition variables (maybe)
              // also, it may be good to set "flushing" when first control message arrives.  perhaps recvRemaining is a good substitute.
              // finally, do we still need to have a UnPop function on the queue? - probably not.
              // even when multiple threads creating control messages, a tag can be handled by 1 thread at a time.

              // here only when all ctrlMsg for the tagged_epoch is complete.
              commLayer.ctrlMsgRemainingForEpoch.erase(te);
              DEBUGF("B Rank %d tag %d epoch %ld start waiting", commLayer.commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);


              // TODO: probably won't work - need multiple worker threads may call lock.  using recrusive is okay but condvar only unlock once.
              commLayer.ctrlMsgProperties.at(MessageTypeInfo::getTag(te)).unlock();

              // so unset 'flushing', and unblock flush() via condition variable.

              commLayer.ctrlMsgProperties.at(MessageTypeInfo::getTag(te)).notifyAll();


              DEBUGF("B rank %d tag %d epoch %ld notified all.", commLayer.commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
            } else {
              DataMessageReceived* msg = dynamic_cast<DataMessageReceived*>(result.second.get());
              // data message.  process it.
              (callbackFunctions[msg->tag])(msg->data.get(), msg->count, msg->rank);
              // delete [] msg.data;  using unique_ptr, don't need to delete msg data.
            }
            // clean up message data
          }
          DEBUGF("B THREAD FINISHED: callback-thread on %d", commLayer.commRank);
        }
    };


public:
    // TODO: leave only code for CommLayer public api

  /**
   * @brief Constructor for the CommLayer.
   * @details have to have the comm_size parameter since we can't get it during member variable initialization and recvQueue needs it.
   *
   * @param communicator    The MPI Communicator used in this communicator
   *                        layer.
   * @param comm_size       The size of the MPI Communicator.
   */
  CommunicationLayer (const MPI_Comm& communicator, const int comm_size)
    : sendQueue(), recvQueue(), recvInProgress(),
      ctrlMsgProperties(), ctrlMsgRemainingForEpoch(),
      sendThread(*this, communicator), recvThread(*this, communicator), callbackThread(*this)
  {
    // init communicator rank and size
    MPI_Comm_size(communicator, &commSize);
    assert(comm_size == commSize);
    MPI_Comm_rank(communicator, &commRank);

  }

  /**
   * @brief Destructor of the CommmunicationLayer.
   * @details Blocks till all threads are finished.
   * @note    If finish() was not called for all used tags, this will block forever.
   */
  virtual ~CommunicationLayer () {
    finalize();
  }

  /**
   * @brief waits for all threads to finish.  this provides a mechanism to ensure all communications
   *    are complete before subsequent MPI calls are made, such as MPI_FINALIZE.
   */
  void finalize() {
    // only 1 thread does this.
//    std::unique_lock<std::recursive_mutex> lock(mutex);

    DEBUGF("M FINALIZING COMMLAYER");

    // TODO: check that we are finished in finishCommunication (turn off sendQueue asap)
	  finishCommunication();

	  // TODO: properly handle this part.
    // wait for both threads to quit
    sendThread.finish();
	  recvThread.finish();
    callbackThread.finish();
  };


  /**
   * @brief Sends a message (raw bytes) to the given rank asynchronously.
   * @details
   *   Asynchronously sends the given message to the given rank with the given
   *   message tag. The message is buffered and batched before send. This function
   *   returns immediately after buffering, which does not guarantee that the
   *   message has been sent yet. Only calling `flush()` guarantees that all
   *   messages are sent, received, and processed before the function returns.
   *
   *   internally, if the buffer if full, it will be enqueued for asynchronous MPI send.
   *
   *   This function is THREAD SAFE
   *
   * @param data        A pointer to the data to be sent.
   * @param count       The number of bytes of the message.
   * @param dst_rank    The rank of the destination processor.
   * @param tag         The tag (type) of the message.
   */
  void sendMessage(const void* data, std::size_t nbytes, int dst_rank, int tag)
  {
    //== don't send control tag.
    assert(tag != CONTROL_TAG && tag >= 0);

    //check to see if sendQueue is still accepting (if not, then comm is finished)
    if (!sendQueue.canPush()) {
      throw std::logic_error("W sendMessage called after finishCommunication");
    }

    //== check to see if the target tag is still accepting
    std::unique_lock<std::recursive_mutex> lock(mutex);

    if (ctrlMsgProperties.count(tag) == 0) {
      lock.unlock();
      throw std::invalid_argument("W invalid tag: tag has not been registered");
    }


    MessageTypeInfo&  tagprop = ctrlMsgProperties.at(tag);
    tagprop.lock();
    lock.unlock();
    //printf("tagprop: tagprop buffers %p, in array %p\n", tagprop.getBuffers(), ctrlMsgProperties.at(tag).getBuffers());

    if (tagprop.isFinished()) {
      tagprop.unlock();
      throw std::invalid_argument("W invalid tag: tag is already FINISHED");
    }

    //== try to append the new data - repeat until successful.
    // along the way, if a full buffer's id is returned, queue it for sendQueue.
    std::pair<bool, BufferPtrType> result;
    do {
      // try append.  append fails if there is no buffer or no room
      result = tagprop.getBuffers()->append(data, nbytes, dst_rank);

      if (result.second) {        // have a full buffer.  implies !result.first
        // verify that the buffer is not empty (may change because of threading)
        if (!(result.second->isEmpty())) {
          // have a non-empty buffer - put in send queue.
        	DEBUGF("Rank %d has full buffer at %p.  enqueue for send.", commRank, result.second);
          std::unique_ptr<SendDataElementType> msg(new SendDataElementType(std::move(result.second), tag, dst_rank));

//          if (sendQueue.isFull()) fprintf(stderr, "Rank %d sendQueue is full for data msgs\n", commRank);

          if (!sendQueue.waitAndPush(std::move(msg)).first) {
            ERROR("W ERROR: sendQueue is not accepting new SendQueueElementType due to disablePush");
            tagprop.unlock();
            throw bliss::io::IOException("W ERROR: sendQueue is not accepting new SendQueueElementType due to disablePush");
          } // else successfully pushed into sendQueue.
        }  // else empty buffer

      } // else don't have a full buffer.

      // repeat until success;
    } while (!result.first);
    tagprop.unlock();
  }


  /**
   * @brief Registers a callback function for the given message tag.
   * @details
   *      The registered callback function will be called for each received message
   *      of the given type (tag). A callback function has to be registered for each
   *      message type sent. Only one callback function can be registered for each
   *      tag, adding more than one results in a `invalid_argument` exception.
   *
   *      A callback function has the signature:
   *         void(uint8_t* msg, std::size_t count, int fromRank)
   *
   * @note  It is STRONGLY ENCOURAGED that the function be THREAD SAFE, as there may be
   *      more than 1 receive thread calling the functions.
   *
   * @param tag                 The message tag for which the callback function
   *                            is registered. Has to be > 0.
   * @param callbackFunction    The callback-function.
   *
   * @throw std::invalid_argument   If `tag <= 0` or if a callback function has
   *                                already been registered for the given tag.
   */
  void addReceiveCallback(int tag, std::function<void(uint8_t*, std::size_t, int)> callbackFunction) throw (std::invalid_argument)
  {
    // check for valid arguments
    assert(tag != CONTROL_TAG && tag >= 0);

    // single thread calls this at a time.
    std::unique_lock<std::recursive_mutex> lock(mutex);

    //=======  reigster the tag.
//    if (ctrlMsgProperties.find(tag) == ctrlMsgProperties.end()) {
//      // register a new tag.
//      ctrlMsgProperties[tag] = std::move(MessageTypeInfo());
//    }

    //== if there isn't already a tag listed, add the MessageBuffers for that tag.
    // multiple threads may call this.
    if (ctrlMsgProperties.count(tag) == 0) {
      // create new message buffer
      //ctrlMsgProperties[tag] = std::move(MessageTypeInfo(tag, commSize));
      ctrlMsgProperties[tag] = std::move(MessageTypeInfo(tag, commSize));
    }
    int64_t te = ctrlMsgProperties[tag].getNextEpoch();
    printf("DONE ADD RECV CALLBACK for tag %d, tagProp tagged_epoch %ld, stored tag %d, epoch %ld\n", tag, te, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
    lock.unlock();

    // now tell the callback thread to register the callbacks.
    callbackThread.addReceiveCallback(tag, callbackFunction);

  }

  /**
   * @brief flushes the message buffers associated with a particular tag.  Blocks until completion
   * @details call by a single thread only. throws exception if another thread is flushing at the same time.
   *        This method sends all buffered messages with a particular tag to all target receivers.
   *        Then it sends a control message to all target receivers and blocks
   *        A process, on receiving control messages from all senders, unblocks the local flush function
   *
   *        The effect is that collective call to flush enforces that all received messages prior
   *        to the last control messages are processed before the flush call is completed.
   *
   *        Each call of this function increments a "epoch" id, which is sent as part of the control
   *        message to all receivers.  Receivers use the epoch id to ensure that one control message
   *        from each sender has been received.
   *
   * @param tag
   * @throw bliss::io::IOException
   */
  void flush(int tag) throw (bliss::io::IOException)
  {
    // multiple threads allowed

    // can only flush data tag.
    assert(tag != CONTROL_TAG && tag >= 0);

    if (!sendQueue.canPush()) {
      throw std::logic_error("W flush called after finishCommunication");
    }

    //== check to see if the target tag is still accepting
    std::unique_lock<std::recursive_mutex> lock(mutex);

    if (ctrlMsgProperties.count(tag) == 0) {
      lock.unlock();
      throw std::invalid_argument("W invalid tag: tag has not been registered");
    }


    MessageTypeInfo& tagprop = ctrlMsgProperties.at(tag);
    tagprop.lock();
    lock.unlock();

    if (tagprop.isFinished()) {
      tagprop.unlock();
      throw std::invalid_argument("W tag not registered, or already finished. cannot FLUSH");
    }

    DEBUGF("M FLUSH Rank %d tag %d", commRank, tag);

    // flush all data buffers (put them into send queue)
    flushBuffers(tag);

    // send the control message - generates an unique epoch for the tag as well.
    // wait till all control messages have been received (not using MPI_Barrier.  see Rationale.)
    DEBUGF("M FLUSH Rank %d tag %d, waiting for control messages to complete", commRank, tag);
    sendControlMessagesAndWait(tag);
    DEBUGF("M FLUSH DONE Rank %d tag %d, finished wait for control messages\n", commRank, tag);

    tagprop.unlock();

  }

  /**
   * @brief stops accepting messages of a particular type and flushes existing messages.  Blocks until completion
   * @details call by a single thread only. throws exception if another thread is flushing at the same time.
   *
   *        This function follows the same logic as flush, (see flush documentation),
   *        except that it also sets the CommLayer NOT to accept messages of that type any further.
   *
   * @note  This does NOT send an application termination message
   *
   * @param tag
   * @throw bliss::io::IOException
   */
  void finish(int tag) throw (bliss::io::IOException)
  {
    assert(tag != CONTROL_TAG && tag >= 0);

    if (!sendQueue.canPush()) {
      //throw std::logic_error("W finish called after finishCommunication");
      return;
    }

    // Mark tag as finished in the metadata.
    // TODO: see if we can change to no lock - at() throws exception, and finish() returns prev value.
    //== check to see if the target tag is still accepting
    std::unique_lock<std::recursive_mutex> lock(mutex);

    if (ctrlMsgProperties.count(tag) == 0) {
      lock.unlock();
      throw std::invalid_argument("W tag not registered. cannot FINISH");
    }


    MessageTypeInfo& tagprop = ctrlMsgProperties.at(tag);
    tagprop.lock();
    lock.unlock();

    if (tagprop.finish()) {  // internally using atomic exchange, so can check to see if already finished.
      lock.unlock();
      //throw std::invalid_argument("W tag not registered, or already finished. cannot FINISH");
      return;
    }

    DEBUGF("M FINISH Rank %d tag %d", commRank, tag);

    // flush all buffers (put them into send queue)
    flushBuffers(tag);

    // send the control message
    // wait till all control messages have been received (not using MPI_Barrier.  see Rationale.)
    DEBUGF("M FINISH Rank %d tag %d, waiting for control messages to complete", commRank, tag);
    sendControlMessagesAndWait(tag);
    DEBUGF("M FINISH Rank %d tag %d, finished wait for control messages", commRank, tag);

    tagprop.unlock();
  }


  /**
   * @brief Initializes all communication and starts the communication and
   *        callback threads.
   *
   * @note Must be called before any other functions can be called.
   */
  void initCommunication()
  {
    // one thread calls this
    if (!sendQueue.canPush()) {
      throw std::logic_error("W initCommunication called after finishCommunication");
    }

    std::unique_lock<std::recursive_mutex> lock(mutex);

    if (ctrlMsgProperties.count(CONTROL_TAG) > 0) {
      lock.unlock();
      throw std::logic_error("W initCommunication called multiple times");
      //return;
    }


    //== init with control_tag only.
    ctrlMsgRemainingForEpoch.emplace(CONTROL_TAGGED_EPOCH, bliss::concurrent::CopyableAtomic<int>(commSize));
    ctrlMsgProperties[CONTROL_TAG] = std::move(MessageTypeInfo(CONTROL_TAG, commSize));

    lock.unlock();

    // TODO: deal with where to put the std::threads.
    callbackThread.start();
    recvThread.start();
    sendThread.start();
  }


  /**
   * @brief stops accepting messages of all types and flushes all existing messages.  Blocks until completion
   * @details call by a single thread only. throws exception if another thread is flushing at the same time.
   *        This method follows the same logic as "finish", but for all message types
   *
   *        At the end, sends an application termination control message.
   *
   * @param tag
   * @throw bliss::io::IOException
   */
  void finishCommunication() throw (bliss::io::IOException)
  {
    DEBUGF("M FINISH COMM rank %d", commRank);


	  // already finishing.
    if (!sendQueue.canPush()) {
      return;
      //throw std::logic_error("W finishCommunication called after finishCommunication");
    }
    std::unique_lock<std::recursive_mutex> lock(mutex);  // not okay to use global mutex.
    ctrlMsgProperties.at(CONTROL_TAG).lock();   // okay to lock wuith control tag, since "finish" uses a different tag.

    // not deleting entries from ctrlMsgProperties, so no need to lock whole thing.
    // go through all tags and "finish" them, except for the final control tag.
	  for (auto ctrlMsgIter = ctrlMsgProperties.begin(); ctrlMsgIter != ctrlMsgProperties.end(); ++ctrlMsgIter) {
      if (ctrlMsgIter->first != CONTROL_TAG && ctrlMsgIter->first >= 0) {
        finish(ctrlMsgIter->first);
      }
    }

    // send the control message for the final control tag.
    // wait till all control messages have been received (not using MPI_Barrier.  see Rationale.)
    DEBUGF("M FINISH COMM Rank %d tag %d, waiting for CONTROL messages to complete", commRank, CONTROL_TAG);
    // ctrlMsgProperties.at(CONTROL_TAG).finish();
    sendControlMessagesAndWait(CONTROL_TAG);  // only 1 control message needs to be sent.
    DEBUGF("M FINISH COMM Rank %d tag %d, finished wait for CONTROL messages", commRank, CONTROL_TAG);

    ctrlMsgProperties.at(CONTROL_TAG).unlock();
    lock.unlock();

  }



  /**
   * @brief Returns the size of the MPI communicator.
   *
   * @return The number of processes that are part of the MPI communicator.
   */
  int getCommSize() const
  {
    return commSize;
  }

  /**
   * @brief Returns the rank of this process in the MPI communicator.
   *
   * @return The rank of this process.
   */
  int getCommRank() const
  {
    return commRank;
  }

protected:


  // TODO: convert to use try-catch of out-of-range exception.
  bool isTagActive(int tag) {
    // single threaded
    std::unique_lock<std::recursive_mutex> lock(mutex);

    return isTagActive(tag, lock);
  }
  bool isTagActive(int tag, std::unique_lock<std::recursive_mutex>& lock) {

    //== check to see if the target tag is still accepting
    if (ctrlMsgProperties.count(tag) == 0) {
      throw std::invalid_argument("W invalid tag: tag has not been registered.");
    }
    return !(ctrlMsgProperties.at(tag).isFinished());
  }

  /**
   * @brief Sends FOC message for the given tag to every MPI process
   *        Blocks till the flusing of the given tag is completed (i.e., all
   *        FOC messages have been received).
   *
   * @param tag     The MPI tag to end.
   * @throw bliss::io::IOException  If the sendQueue has been disabled.
   * @return bool indicating if control messages were successfully sent.
   */
  bool sendControlMessagesAndWait(int tag) throw (bliss::io::IOException)
  {
    if (!sendQueue.canPush())
      throw bliss::io::IOException("M ERROR: sendControlMessagesAndWait already called with CONTROL_TAG");



    // since each tag has unique epoch for each call to this function, we don't need global lock as much.
    TaggedEpoch te = ctrlMsgProperties.at(tag).getNextEpoch();

    //ctrlMsgProperties.at(tag).lock();   // for loop may get out of order.  is that okay? yes.  but sendQueue being disabled should be critical.
    if (tag != CONTROL_TAG) {
      // not a control tag, so don't need to add to recvRemaining.

      // lock it for the tag - no multiple concurrent calls with the same tag.
      ctrlMsgRemainingForEpoch.emplace(te, bliss::concurrent::CopyableAtomic<int>(commSize));  // insert new, and proceed to sending messages
      DEBUGF("M Rank %d tag %d epoch %ld added tag to recvRemaining.", commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
    } // else there is only one epoch for CONTROL_TAG, and it's already added to the list.


    // should be only 1 thread executing this code below with the provided tag.

    for (int i = 0; i < getCommSize(); ++i) {
      // send end tags in circular fashion, send tag to self last
      int target_rank = (i + getCommRank() + 1) % getCommSize();

      //DEBUGF("M Rank %d sendEndTags %d epoch %d target_rank = %d ", commRank, te.tag, te.epoch, target_rank );

      // send the end message for this tag.
      std::unique_ptr<ControlMessage> msg(new ControlMessage(te, target_rank));


     // if (sendQueue.isFull()) fprintf(stderr, "Rank %d sendQueue is full for control msgs\n", commRank);
      if (!sendQueue.waitAndPush(std::move(msg)).first) {
        //ctrlMsgProperties.at(tag).unlock();
        throw bliss::io::IOException("M ERROR: sendQueue is not accepting new SendQueueElementType due to disablePush");
      }
    }


    // if we are sending the control tag, then afterward we are done.  mixed CONTROL_TAG and others could cause sendQueue to be prematurely disabled.
    if (tag == CONTROL_TAG)
      sendQueue.disablePush();
//    ctrlMsgProperties.at(tag).unlock();


    //============= now wait.

    // CONCERN:  tracking the tag being flushed using a shared "flushing" opens up the possible problem
    // when out-of-order calls to flush, finish, and finishCommunications.


    // waiting for all messages of this tag to flush.
    //DEBUGF("M Rank %d Waiting for tag %d", commRank, te.tag);


    // TODO:  possible to have ctrlMsgRemainingForEpoch be atomic and use atomic load and fetch_sub to avoid the lock here?
//    ctrlMsgProperties.at(tag).lock();
    while (ctrlMsgRemainingForEpoch.count(te) > 0) {
      // condition variable waits for notification, which is generated by recv thread
      // when all FOC messages with this tag are received.
      DEBUGF("M PRE WAIT Rank %d Waiting for tag %d epoch %ld, currently flushing", commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
      ctrlMsgProperties.at(tag).wait();
      DEBUGF("M END WAIT Rank %d Waiting for tag %d epoch %ld, currently flushing", commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
    }
//    ctrlMsgProperties.at(tag).unlock();

    DEBUGF("M Rank %d received all END message for TAG %d, epoch = %ld", commRank, MessageTypeInfo::getTag(te), te & 0x00000000FFFFFFFF);
    return true;
  }


  /**
   * @brief Flushes all data buffers for message tag `tag`.
   * @details All pending/buffered messages of type `tag` will be send out.
   *    enqueues data buffers to be sent, but enqueuing the buffer's id, if buffer is not empty.
   *
   * @param tag The message tag, whose messages are flushed.
   * @throw bliss::io::IOException
   */
  void flushBuffers(int tag) throw (bliss::io::IOException)
  {

    if (!sendQueue.canPush()) {
      throw std::logic_error("W cannot flush buffer: cannot push to sendQueue.");
    }
    if (ctrlMsgProperties.count(tag) == 0) {
      throw std::logic_error("W cannot flush buffer: unknown tag.");
    }

    //ctrlMsgProperties.at(tag).lock();

    for (int i = 0; i < commSize; ++i) {
      // flush buffers in a circular fashion, starting with the next neighbor
      int target_rank = (i + getCommRank() + 1) % getCommSize();

      auto bufPtr = ctrlMsgProperties.at(tag).getBuffers()->flushBufferForRank(target_rank);
      // flush/send all remaining non-empty buffers
      if ((bufPtr) && !(bufPtr->isEmpty())) {
        std::unique_ptr<SendDataElementType> msg(new SendDataElementType(std::move(bufPtr), tag, target_rank));

//        if (sendQueue.isFull()) fprintf(stderr, "Rank %d sendQueue is full for flushBuffers\n", commRank);

        if (!sendQueue.waitAndPush(std::move(msg)).first) {
//          ctrlMsgProperties.at(tag).unlock();
          throw bliss::io::IOException("M ERROR: sendQueue is not accepting new SendQueueElementType due to disablePush");
        }
      }
    }

//    ctrlMsgProperties.at(tag).unlock();
  }




protected:
/******************************************************************************
 *                           Protected member fields                            *
 ******************************************************************************/


  /* Thread-safe queues for posting sends and getting received messages,
   * these are used as communication medium between the comm-thread and the
   * (potetially multiple) producer threads for the sends; and the comm-thread
   * and the callback handler thread for the receives
   */

  /// Mutex for adding/removing from local arrays and maps.
  mutable std::recursive_mutex mutex;

  /// atomic variable indicating if the CommLayer has been finalized (all communications are completed, ready for destruction.)
//  std::atomic<bool> finalized;


  /* information per message tag */

  // TODO: comments about when they are used.

  /**
   *  Message buffers per message tag (maps each tag to a set of buffers, one buffer for each target rank)
   *  only destroyed at the end, after all sends are done.
   */
  std::unordered_map<int, std::atomic<MessageBuffersType*> > buffers;


  /**
   *  map between tag and MessageTypeInfo, which contains info about next epoch id,
   *  mutex, and condition variables.  Also has flag to indicate if tag is "finished"
   *
   *  use the flag instead of deleting the MessageTypeInfo because it's easier for other threads to check the flag.
   *
   *  Ensures that during sendControlMessageAndWait, a single thread is changing the condition for the wait.
   *
   */
  std::unordered_map<int, MessageTypeInfo > ctrlMsgProperties;

  // each tag-epoch pair has a count, so each MPI process can check to see how many from that epoch is still remaining.
  /// Per tag: number of processes that haven't sent the END-TAG message yet
  //std::unordered_map<TaggedEpoch, int, TaggedEpochHash, TaggedEpochEqual> ctrlMsgRemainingForEpoch;
  std::unordered_map<TaggedEpoch, bliss::concurrent::CopyableAtomic<int> > ctrlMsgRemainingForEpoch;

  // send thread and recv thread should be separate, else we could have a race condition.
  // e.g. send locally, recvInProgres queue is full, so send function blocks,
  // but recv function can be called only after send function returns, so queue can't clear.
  // also, it's just cleaner.
  SendCommThread sendThread;
  RecvCommThread recvThread;

  // call back thread to process the received objects.
  CallbackThread callbackThread;

  /// The MPI Communicator size
  int commSize;

  /// The MPI Communicator rank
  int commRank;

};





} // namespace io
} // namespace bliss

#endif // BLISS_COMMUNICATION_LAYER_HPP
