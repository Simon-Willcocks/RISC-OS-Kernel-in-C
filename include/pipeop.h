/* Copyright 2022 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Inline routines to access OS_PipeOp pipes.
 */


#ifndef __KERNEL_H
typedef struct {
  error_block *error;
  void *location;
  uint32_t available; 
} PipeSpace;

#define OS_PipeOp 0x200fa
#endif

#ifndef NOT_DEBUGGING
static inline 
#endif
uint32_t PipeOp_CreateForTransfer( uint32_t max_block )
{
  register uint32_t max_block_size asm ( "r1" ) = max_block;
  register uint32_t max_data asm ( "r2" ) = 0; // Unlimited
  register uint32_t allocated_mem asm ( "r3" ) = 0; // OS allocated

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "svc %[swi]" 
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc", "memory"
        );

  return pipe;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
uint32_t PipeOp_CreateOnBuffer( void *buffer, uint32_t len )
{
  // Fixed length pipe
  register uint32_t max_block_size asm ( "r1" ) = len;
  register uint32_t max_data asm ( "r2" ) = len;
  register void *allocated_mem asm ( "r3" ) = buffer;

  register uint32_t pipe asm ( "r0" );

  asm volatile ( "svc %[swi]" 
             "\n  movvs R0, #0"
        : "=r" (pipe)
        : [swi] "i" (OSTask_PipeCreate)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr", "cc", "memory"
        );

  return pipe;
}

// This routine will return immediately if the requested space exceeds the capacity.
// This routine will return early if NotListening (-> space.location = 0) is called
// Data consumers, if they want to consume fixed-sized blocks at a time, should allocate at least one
// extra block of capacity
// This is complicated. I want the data to be aligned properly so that it can be sent to a device in
// pages, but if I have a few bytes in the pipe, then the writer wants to write the total capacity,
// it should be able to wait for that much space.
// Maybe the writer should be told how many (more) bytes the reader is waiting for, instead? But that's
// likely to be one byte.
// Allocate capacity + 1 block, report capacity each time.
#ifndef NOT_DEBUGGING
static inline 
#endif
PipeSpace PipeOp_WaitForSpace( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForSpace)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
PipeSpace PipeOp_SpaceFilled( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  // In this case, bytes represents the number of bytes that the caller has written and
  // is making available to the reader.
  // The returned information is the same as from WaitForSpace and indicates the remaining
  // space after the filled bytes have been accepted. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last accepted byte.
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeSpaceFilled)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
PipeSpace PipeOp_WaitForData( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeWaitForData)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
PipeSpace PipeOp_DataConsumed( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  // In this case, the bytes are the number of bytes no longer of interest.
  // The returned information is the same as from WaitForData and indicates the remaining
  // data after the consumed bytes have been removed. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last consumed byte.
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t amount asm ( "r1" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r1" );
  register void *location asm ( "r2" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OSTask_PipeDataConsumed)
        , "r" (pipe)
        , "r" (amount)
        : "lr", "cc", "memory"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
error_block *PipeOp_SetReceiver( uint32_t read_pipe, uint32_t new_receiver )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;
  register uint32_t task asm ( "r1" ) = new_receiver;

  // OUT
  register error_block *error asm ( "r0" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeSetReceiver)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc", "memory"
        );

  return error;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
error_block *PipeOp_SetSender( uint32_t write_pipe, uint32_t new_sender )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = write_pipe;
  register uint32_t task asm ( "r1" ) = new_sender;

  // OUT
  register error_block *error;

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc %[error], #0"
    "\n  movvs %[error], r0"

        : [error] "=r" (error)
        : [swi] "i" (OSTask_PipeSetSender)
        , "r" (pipe)
        , "r" (task)
        : "lr", "cc", "memory"
        );

  return error;
}

#ifndef NOT_DEBUGGING
static inline 
#endif
error_block *PipeOp_NotListening( uint32_t read_pipe )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = read_pipe;

  // OUT
  register error_block *error asm ( "r0" );

  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNotListening)
        , "r" (pipe)
        : "lr", "cc", "memory"
        );

  return error;
}


#ifndef NOT_DEBUGGING
static inline 
#endif
error_block *PipeOp_NoMoreData( uint32_t send_pipe )
{
  // IN
  register uint32_t pipe asm ( "r0" ) = send_pipe;

  // OUT
  register error_block *error asm ( "r0" );

  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OSTask_PipeNoMoreData)
        , "r" (pipe)
        : "lr", "cc", "memory"
        );

  return error;
}

